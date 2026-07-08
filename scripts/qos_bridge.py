#!/usr/bin/env python3
"""
QuantumOS host-side bridge (epic #99): the ONE implementation of the COM2
swarm-bridge wire protocol, the Lamport boot-attestation verifier, and the
QEMU VM lifecycle manager that the MCP server (scripts/qos_mcp.py) and the
CI test (scripts/test_qos_mcp.py) are built on. verify_attestation.py and
swarm_pingpong.py import their framing/verify from here, so there is exactly
one CRC8/frame/verify implementation — no wire drift.

Standard library only (hashlib, socket, subprocess, threading, ...): the
integration-CI python has no `mcp` package, and this module must never import
it. `mcp` lives only in qos_mcp.py.

Wire framing (little-endian), mirrored from user/swarm.h:
    0xA5 | type:u8 | len:u16 | payload[len] | crc8
CRC-8/CCITT (poly 0x07, init 0x00, MSB-first) over type+len+payload.

TRUST BOUNDARY (read before believing `verified`): the Lamport signature
covers ONLY the boot ATTEST string. It proves "a QuantumOS boot with this
qseed once produced this attestation", NOT liveness (a captured attestation
replays — the wire protocol carries no host challenge/nonce) and NOT the
authenticity of any later STATUS/RECALL reply (DATA frames are unsigned).
Trust therefore equals control of the serial channel — the same class of
trust as holding the console. identity() says so; the MCP tool docstrings
repeat it.
"""

import atexit
import ctypes
import hashlib
import os
import platform
import re
import signal
import socket
import subprocess
import threading
import time
import uuid

# ---- wire constants (user/swarm.h) ----------------------------------------
MAGIC = 0xA5
SWARM_MAX_PAYLOAD = 512

FRAME_HANDSHAKE = 0x01
FRAME_DATA = 0x02
FRAME_PING = 0x03
FRAME_PONG = 0x04
FRAME_DISCONNECT = 0x05
FRAME_PKDIGEST = 0x10
FRAME_ATTEST = 0x11
FRAME_SIG = 0x12

SWARM_OP_STATUS = 0x01
SWARM_OP_RECALL = 0x02

# ---- Lamport parameters (user/swarm.h) ------------------------------------
LAMPORT_BITS = 256
HASH_LEN = 32
SIG_ELEM = 64                        # revealed preimage (32) + comp pk hash (32)
SIG_LEN = LAMPORT_BITS * SIG_ELEM    # 16384


# ============================================================================
# Framing
# ============================================================================
def crc8(data: bytes) -> int:
    """CRC-8/CCITT (poly 0x07, init 0x00, MSB-first) — matches swarm_crc8()."""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def frame(ftype: int, payload: bytes) -> bytes:
    """Build one wire frame."""
    hdr = bytes([ftype, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
    return bytes([MAGIC]) + hdr + bytes([crc8(hdr)])


class FrameParser:
    """Streaming frame extractor for a long-lived COM2 stream.

    Resyncs on a non-magic byte or a bad CRC by dropping one byte (matching
    swarm_pingpong.parse and the guest's poll_com2). Two hardening rules the
    persistent-client case needs that the one-shot scripts lacked:

      * `bad_crc` is counted (verify_attestation's gate hard-fails on it), and
      * a length field > SWARM_MAX_PAYLOAD is rejected by dropping the magic
        byte and resyncing (the guest's rule, swarm_svc.c). Without it a single
        flipped length byte makes the parser wait forever for len+5 bytes that
        never come, wedging every subsequent reply until VM reboot.
    """

    def __init__(self):
        self.buf = bytearray()
        self.bad_crc = 0
        self.bad_len = 0

    def feed(self, data: bytes):
        """Append bytes; return a list of (type, payload) for complete frames."""
        self.buf.extend(data)
        out = []
        while True:
            while self.buf and self.buf[0] != MAGIC:
                del self.buf[0]
            if len(self.buf) < 4:
                return out
            length = self.buf[2] | (self.buf[3] << 8)
            if length > SWARM_MAX_PAYLOAD:
                self.bad_len += 1
                del self.buf[0]      # impossible length: resync past this magic
                continue
            total = 4 + length + 1
            if len(self.buf) < total:
                return out           # frame not fully arrived yet
            if crc8(bytes(self.buf[1:4 + length])) == self.buf[4 + length]:
                out.append((self.buf[1], bytes(self.buf[4:4 + length])))
                del self.buf[:total]
            else:
                self.bad_crc += 1
                del self.buf[0]      # resync past this magic


def parse_frames(blob: bytes):
    """Whole-blob variant used by verify_attestation's CLI.

    Returns (frames, bad_crc). Behaviour on a clean capture is identical to
    the historical parse_frames; it additionally resyncs past an impossible
    length instead of truncating the rest of the stream.
    """
    p = FrameParser()
    frames = p.feed(blob)
    return frames, p.bad_crc


# ============================================================================
# Lamport attestation verification
# ============================================================================
def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def verify_lamport(message: bytes, pkdigest: bytes, signature: bytes) -> bool:
    """Rebuild the public key from the signature and check it against the
    committed digest. One digest equation catches a tampered signature,
    message, or committed digest alike."""
    if len(signature) != SIG_LEN:
        return False
    if len(pkdigest) != HASH_LEN:
        return False
    md = sha256(message)
    pk_stream = bytearray()
    for i in range(LAMPORT_BITS):
        bit = (md[i >> 3] >> (i & 7)) & 1
        elem = signature[i * SIG_ELEM:(i + 1) * SIG_ELEM]
        preimage = elem[:HASH_LEN]
        comp = elem[HASH_LEN:]
        pk = [None, None]
        pk[bit] = sha256(preimage)   # pk[i][bit]
        pk[1 - bit] = comp           # pk[i][1-bit], revealed in the signature
        pk_stream += pk[0]
        pk_stream += pk[1]
    return sha256(bytes(pk_stream)) == pkdigest


def parse_attestation(msg: str):
    """Parse 'QOS-BOOT|qseed=<hex|none>|ticks=<n>' -> (qseed_or_None, ticks)."""
    parts = msg.split("|")
    if len(parts) != 3 or parts[0] != "QOS-BOOT":
        raise ValueError(f"malformed attestation: {msg!r}")
    if not parts[1].startswith("qseed=") or not parts[2].startswith("ticks="):
        raise ValueError(f"malformed attestation fields: {msg!r}")
    qseed = parts[1][len("qseed="):]
    ticks = parts[2][len("ticks="):]
    qseed_val = None if qseed == "none" else int(qseed, 16)
    return qseed_val, int(ticks)


def expected_qseed(arg):
    """Normalise a requested qseed (hex string, 'none', or None) to int|None."""
    if arg is None:
        return None
    if isinstance(arg, int):
        return arg
    return None if arg.lower() == "none" else int(arg, 16)


class Attestation:
    """The verified (or refused) boot identity of one VM."""

    def __init__(self, verified, reason, qseed, ticks, pkdigest, raw):
        self.verified = verified
        self.reason = reason
        self.qseed = qseed          # int | None
        self.ticks = ticks
        self.pkdigest = pkdigest    # bytes | None
        self.raw = raw              # the captured attestation bytes

    def qseed_hex(self):
        return "none" if self.qseed is None else format(self.qseed, "X")

    def to_dict(self, boot_nonce=None):
        return {
            "verified": self.verified,
            "reason": self.reason,
            "qseed": self.qseed_hex(),
            "ticks": self.ticks,
            "pkdigest": self.pkdigest.hex() if self.pkdigest else None,
            "boot_nonce": boot_nonce,
            "trust_note": (
                "verified == attestation frames intact + Lamport signature "
                "consistent + attested qseed matches request. NOT liveness "
                "(a captured attestation replays; the wire protocol carries no "
                "host nonce) and NOT per-operation authentication (STATUS/"
                "RECALL replies are unsigned). Trust == control of the serial "
                "channel, same class as the console."
            ),
        }


def attestation_from_bytes(blob: bytes, requested_qseed=None) -> Attestation:
    """Parse+verify a full captured attestation byte stream. This is the SAME
    code path boot() uses, so the CI tamper test can flip a byte in a captured
    blob and get the identical verdict the live tools would."""
    frames, bad_crc = parse_frames(blob)
    if bad_crc:
        return Attestation(False, f"{bad_crc} frame(s) with bad CRC8", None, None, None, blob)

    pkdigest = None
    attest_msg = None
    sig = bytearray()
    for ftype, payload in frames:
        if ftype == FRAME_PKDIGEST:
            pkdigest = bytes(payload)
        elif ftype == FRAME_ATTEST:
            attest_msg = payload.decode("ascii", errors="replace")
        elif ftype == FRAME_SIG:
            sig += payload

    if pkdigest is None or attest_msg is None or not sig:
        return Attestation(False, "missing pk-digest, attestation, or signature",
                           None, None, pkdigest, blob)
    try:
        qseed_val, ticks = parse_attestation(attest_msg)
    except ValueError as exc:
        return Attestation(False, str(exc), None, None, pkdigest, blob)

    if not verify_lamport(attest_msg.encode("ascii"), pkdigest, bytes(sig)):
        return Attestation(False, "Lamport signature invalid", qseed_val, ticks, pkdigest, blob)

    want = expected_qseed(requested_qseed)
    if qseed_val != want:
        got = "none" if qseed_val is None else format(qseed_val, "X")
        exp = "none" if want is None else format(want, "X")
        return Attestation(False, f"attested qseed {got} != requested {exp}",
                           qseed_val, ticks, pkdigest, blob)

    return Attestation(True, "ok", qseed_val, ticks, pkdigest, blob)


def read_boot_attestation(recv_fn, deadline, requested_qseed=None) -> Attestation:
    """Accumulate COM2 bytes via recv_fn() until the SIG stream is complete (or
    the deadline passes), then verify. recv_fn() returns bytes, None when
    nothing arrived before its own timeout, or b'' on EOF (VM gone)."""
    parser = FrameParser()
    raw = bytearray()
    have_pk = have_attest = False
    sig_len = 0
    while time.time() < deadline:
        chunk = recv_fn()
        if chunk is None:
            continue
        if chunk == b"":
            break
        raw.extend(chunk)
        for ftype, payload in parser.feed(chunk):
            if ftype == FRAME_PKDIGEST:
                have_pk = True
            elif ftype == FRAME_ATTEST:
                have_attest = True
            elif ftype == FRAME_SIG:
                sig_len += len(payload)
        if have_pk and have_attest and sig_len >= SIG_LEN:
            break
    return attestation_from_bytes(bytes(raw), requested_qseed)


# ============================================================================
# QosVM — the QEMU lifecycle + bridge client
# ============================================================================
class QosError(Exception):
    """Base for structured tool failures (serialized by qos_mcp)."""


class QosRefused(QosError):
    """The VM's attestation is not verified — field/run tools refuse."""


class QosDead(QosError):
    """The VM process has exited."""


class QosTimeout(QosError):
    """A bridge reply or qsh marker did not arrive in time."""


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _default_kernel():
    """The standard build path, or — if the artifact landed elsewhere (the CI
    integration job unpacks it under an unpredictable build/ subdir) — the
    first kernel.elf32 found under build/."""
    root = _repo_root()
    std = os.path.join(root, "build", "x86_64", "kernel.elf32")
    if os.path.exists(std):
        return std
    build = os.path.join(root, "build")
    for dirpath, _dirs, files in os.walk(build):
        if "kernel.elf32" in files:
            return os.path.join(dirpath, "kernel.elf32")
    return std  # not found — boot() raises a clear error naming this path


QSH_BANNER = "QSH: QuantumOS interactive shell ready"
_ALLOWED_RUN = re.compile(r"^/bin/[A-Za-z0-9_]+$")


class QosVM:
    """One QuantumOS VM. COM1 (=stdio) carries the boot log and the scripted
    qsh session; COM2 (=a TCP socket QEMU connects OUT to, so byte 0 of the
    one-shot attestation is captured and there is no free-port TOCTOU race)
    carries the attested bridge. A daemon thread drains COM1 the instant QEMU
    starts (or a full stdout pipe deadlocks QEMU mid-attestation). One coarse
    lock serializes every bridge transaction and qsh cycle, because FastMCP
    dispatches tool calls on a threadpool."""

    def __init__(self, kernel=None):
        self.kernel = kernel or _default_kernel()
        self.proc = None
        self.attestation = None
        self.boot_nonce = None
        self._com2 = None
        self._listen = None
        self._parser = FrameParser()
        self._log = bytearray()
        self._log_lock = threading.Lock()
        self._io_lock = threading.RLock()
        self._drain = None
        self._atexit_armed = False

    # -- COM1 drain ---------------------------------------------------------
    def _drain_stdout(self):
        f = self.proc.stdout
        while True:
            chunk = f.read(4096)
            if not chunk:
                return
            with self._log_lock:
                self._log.extend(chunk)

    def _log_text(self, start=0):
        with self._log_lock:
            return self._log[start:].decode("utf-8", errors="replace")

    def _log_len(self):
        with self._log_lock:
            return len(self._log)

    # -- lifecycle ----------------------------------------------------------
    def boot(self, qseed=None, timeout=30):
        if self.proc is not None:
            raise QosError("a VM is already running — shut it down first")
        if not os.path.exists(self.kernel):
            raise QosError(f"kernel image not found: {self.kernel}")

        # Host listens; QEMU connects OUT to this port for COM2.
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.bind(("127.0.0.1", 0))
        self._listen.listen(1)
        self._listen.settimeout(timeout)
        port = self._listen.getsockname()[1]

        cmd = ["qemu-system-x86_64", "-kernel", self.kernel]
        if qseed:
            cmd += ["-append", f"qseed={qseed}"]
        cmd += [
            "-serial", "stdio",                          # COM1: console + qsh
            "-serial", f"tcp:127.0.0.1:{port}",          # COM2: attested bridge
            "-m", "128M", "-display", "none", "-no-reboot",
        ]

        preexec = _pdeathsig if platform.system() == "Linux" else None
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0, preexec_fn=preexec,
        )
        self.boot_nonce = f"{uuid.uuid4().hex}-{time.monotonic_ns()}"
        self._arm_atexit()

        # Drain COM1 immediately, before any blocking COM2 read.
        self._drain = threading.Thread(target=self._drain_stdout, daemon=True)
        self._drain.start()

        # Accept QEMU's COM2 connection (it connects during startup).
        try:
            self._com2, _ = self._listen.accept()
        except socket.timeout:
            self.shutdown()
            raise QosTimeout("QEMU never connected COM2 (boot failed?)")
        finally:
            self._listen.close()
            self._listen = None
        self._com2.settimeout(1.0)

        # Read + verify the boot attestation (independent deadline from COM1).
        att_deadline = time.time() + timeout
        self.attestation = read_boot_attestation(self._recv_com2, att_deadline, qseed)

        # Wait for qsh readiness on COM1 (its own deadline — the two services
        # are independent and arrive in either order).
        self._await_banner(time.time() + timeout)
        # Let the transient boot citizens (init, canaries) finish before any
        # `run` spawns a citizen: an early spawn races the initrd/scheduler
        # settling and can load a stillborn process. Observable, bounded.
        self._settle(time.time() + 5)
        return self.identity()

    def _recv_com2(self):
        """Bytes on data, None on a read timeout (nothing yet), b'' on EOF."""
        if self.proc and self.proc.poll() is not None:
            return b""
        try:
            return self._com2.recv(4096)
        except socket.timeout:
            return None
        except OSError:
            return b""

    def _await_banner(self, deadline):
        while time.time() < deadline:
            if QSH_BANNER in self._log_text():
                return
            if self.proc.poll() is not None:
                raise QosDead("VM exited before qsh came up")
            time.sleep(0.1)
        raise QosTimeout("qsh banner never appeared on COM1")

    def _settle(self, deadline):
        """Wait for the boot self-test citizens to finish (the canary
        'ISOLATION VERIFIED' line marks the transient spawns done), so a
        later `run` doesn't race early boot. Bounded — proceeds at the
        deadline regardless."""
        while time.time() < deadline:
            if "ISOLATION VERIFIED" in self._log_text():
                return
            if self.proc.poll() is not None:
                raise QosDead("VM exited during boot settle")
            time.sleep(0.1)

    def _arm_atexit(self):
        if self._atexit_armed:
            return
        atexit.register(self.shutdown)
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                signal.signal(sig, self._signal_shutdown)
            except (ValueError, OSError):
                pass  # not the main thread / unsupported — atexit still covers
        self._atexit_armed = True

    def _signal_shutdown(self, *_):
        self.shutdown()
        os._exit(1)

    def shutdown(self):
        """Idempotent: SIGTERM -> grace -> SIGKILL -> reap. qsh `exit` only
        triggers a watchdog rebirth, never a poweroff, so killing QEMU is the
        only clean stop."""
        proc, self.proc = self.proc, None
        if self._com2 is not None:
            try:
                self._com2.close()
            except OSError:
                pass
            self._com2 = None
        if self._listen is not None:
            try:
                self._listen.close()
            except OSError:
                pass
            self._listen = None
        if proc is None:
            return {"stopped": True}
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
        return {"stopped": True}

    def is_running(self):
        return self.proc is not None and self.proc.poll() is None

    # -- identity + guards --------------------------------------------------
    def identity(self):
        if self.attestation is None:
            return {"verified": False, "reason": "no VM booted", "boot_nonce": None}
        return self.attestation.to_dict(self.boot_nonce)

    def _ensure_verified(self):
        if self.attestation is None or not self.attestation.verified:
            reason = self.attestation.reason if self.attestation else "no VM booted"
            raise QosRefused(f"attestation not verified: {reason}")
        if self.proc is not None and self.proc.poll() is not None:
            raise QosDead("VM has exited")

    # -- COM2 bridge ops ----------------------------------------------------
    def _transact(self, req_frame, expect_op, deadline):
        """Send one DATA request and return the matching-opcode reply payload."""
        with self._io_lock:
            self._com2.sendall(req_frame)
            while time.time() < deadline:
                chunk = self._recv_com2()
                if chunk is None:
                    continue
                if chunk == b"":
                    raise QosDead("COM2 closed (VM gone)")
                for ftype, payload in self._parser.feed(chunk):
                    if ftype == FRAME_DATA and payload and payload[0] == expect_op:
                        return payload
                    # stray PONG / late attestation frame / other op: ignore
        raise QosTimeout(f"no DATA reply for op {expect_op}")

    def status(self, deadline_s=5.0):
        """ghostd field STATUS over COM2: {r, live}. (This is the ghostd
        Hopfield field, distinct from qsh's kernel holographic field.)"""
        self._ensure_verified()
        payload = self._transact(frame(FRAME_DATA, bytes([SWARM_OP_STATUS])),
                                 SWARM_OP_STATUS, time.time() + deadline_s)
        r_q16 = int.from_bytes(payload[1:5], "little") if len(payload) >= 5 else 0
        live = payload[5] if len(payload) > 5 else 0
        return {"r": r_q16 / 65536.0, "live": live, "identity": self.identity()}

    # -- COM1 scripted qsh --------------------------------------------------
    def _send_line(self, line):
        """Write one command to qsh; return the log offset just before it (so
        a subsequent wait only sees output this command produced). Caller
        holds _io_lock."""
        if not self.is_running():
            raise QosDead("VM has exited")
        start = self._log_len()
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        return start

    def _wait_markers(self, start, markers, deadline_s, what):
        """Poll the COM1 log from `start` until one of the regex `markers`
        matches; return (text, match). Caller holds _io_lock."""
        deadline = time.time() + deadline_s
        while time.time() < deadline:
            text = self._log_text(start)
            for pat in markers:
                m = pat.search(text)
                if m:
                    return text, m
            if self.proc.poll() is not None:
                raise QosDead("VM exited mid-command")
            time.sleep(0.05)
        raise QosTimeout(f"{what}: no completion marker")

    def _qsh(self, line, markers, deadline_s):
        with self._io_lock:
            start = self._send_line(line)
            return self._wait_markers(start, markers, deadline_s, repr(line))

    def imprint(self, text, deadline_s=10.0):
        """Kernel holographic field imprint via qsh (same space as a human at
        the prompt). Returns the assigned slot."""
        self._ensure_verified()
        _ok = re.compile(r"FIELD: imprinted slot (\d+)")
        _err = re.compile(r"qsh: imprint(?: failed| : usage|:)")
        _txt, m = self._qsh("imprint " + text, (_ok, _err), deadline_s)
        if m.re is _err:
            raise QosError("imprint rejected by qsh")
        return {"slot": int(m.group(1)), "identity": self.identity()}

    def recall(self, text, deadline_s=10.0):
        """Kernel holographic field recall via qsh. Returns the winner text."""
        self._ensure_verified()
        _win = re.compile(r'FIELD: winner="(.*?)" slot=(\d+) score=(-?\d+) n=(\d+)')
        _empty = re.compile(r"FIELD: recall empty")
        _err = re.compile(r"qsh: recall(?: failed|:)")
        _txt, m = self._qsh("recall " + text, (_win, _empty, _err), deadline_s)
        if m.re is _win:
            return {"winner": m.group(1), "slot": int(m.group(2)),
                    "score": int(m.group(3)), "n": int(m.group(4)),
                    "identity": self.identity()}
        return {"winner": None, "slot": None, "score": None, "n": 0,
                "identity": self.identity()}

    def run(self, program, deadline_s=20.0):
        """Spawn a /bin citizen via qsh and collect its output + exit code.
        Matches the SPECIFIC spawned pid's exit line, so an interleaved boot
        process exiting can't be mistaken for this program's completion."""
        self._ensure_verified()
        if not _ALLOWED_RUN.match(program):
            raise QosError(f"program must match /bin/<name>: {program!r}")
        with self._io_lock:
            start = self._send_line("run " + program)
            _spawned = re.compile(r"qsh: spawned pid (\d+)")
            _nostart = re.compile(r"qsh: run: cannot start")
            _text, m = self._wait_markers(start, (_spawned, _nostart), deadline_s,
                                          f"run {program}")
            if m.re is _nostart:
                raise QosError(f"qsh could not start {program}")
            pid = m.group(1)
            _done = re.compile(r"qsh: pid " + pid + r" exited \(code (\d+)\)")
            _gone = re.compile(r"qsh: pid " + pid + r" (?:still running|vanished)")
            text, m = self._wait_markers(start, (_done, _gone), deadline_s,
                                         f"run {program} completion")
            code = int(m.group(1)) if m.re is _done else None
        return {"output": text.strip(), "exit_code": code, "identity": self.identity()}


def _pdeathsig():
    """Linux: die with the parent so a killed MCP process never orphans QEMU."""
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        PR_SET_PDEATHSIG = 1
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
    except OSError:
        pass
