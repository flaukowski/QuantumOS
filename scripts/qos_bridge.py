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


# ============================================================================
# qsh input validation — the console is a line protocol
# ============================================================================
# qsh's line editor (user/qsh.c:1392) accepts ONLY printable ASCII and silently
# DROPS bytes past LINE_MAX-1 = 119, then executes the truncated PREFIX on Enter
# (qsh.c:1377). So a control byte (\n / \r) in an argument would run a SECOND
# command, and an over-long argument would run a DIFFERENT command. Every value
# spliced into a qsh line therefore passes through _qsh_text/_qsh_path first.
# The byte budget is security-critical, not cosmetic. Rejecting the reserved
# marker prefixes as well stops agent-controlled text from impersonating a
# result line (the host parses those prefixes as ground truth).
QSH_LINE_MAX = 119
_QSH_MARKERS = ("qsh:", "FIELD:", "PS:", "MEM:", "TIME:")


def _qsh_text(value, budget, what="argument"):
    """Validate one argument destined for a qsh command line. Returns it
    unchanged or raises QosError. `budget` is the byte room left on the line
    after the fixed command prefix (spaces included). The charset check runs
    FIRST so the length check can encode as ASCII without failing, and so
    byte-length == char-length holds against qsh's 119-byte line cap."""
    if not isinstance(value, str):
        raise QosError(f"{what} must be a string")
    for ch in value:
        o = ord(ch)
        if o < 0x20 or o > 0x7E:
            raise QosError(
                f"{what} contains non-printable byte 0x{o:02X}; only ASCII "
                f"0x20-0x7E is legal on the qsh line (a control byte would "
                f"inject a second command)")
    n = len(value.encode("ascii"))
    if n > budget:
        raise QosError(
            f"{what} too long: {n} > {budget} bytes; qsh drops input past "
            f"{QSH_LINE_MAX} and would execute a truncated, different command")
    stripped = value.lstrip()
    for mk in _QSH_MARKERS:
        if stripped.startswith(mk):
            raise QosError(
                f"{what} may not begin with the reserved result marker {mk!r}")
    return value


_QSH_PATH = re.compile(r"^/?[A-Za-z0-9._][A-Za-z0-9._/\-]{0,62}$")


def _qsh_path(value):
    """Validate a filesystem path argument: printable, bounded, no space (qsh
    splits the command on the first space), no `..` traversal past the flat
    ramfs/initrd namespace (kernel/src/ramfs.c:30-37 strips ./ and leading /)."""
    _qsh_text(value, QSH_LINE_MAX, "path")
    if " " in value or ".." in value:
        raise QosError(f"invalid path {value!r} (no spaces or '..')")
    if not _QSH_PATH.match(value):
        raise QosError(f"invalid path {value!r}")
    return value


_FETCH_HOST = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.\-]{0,62}$")


def _is_private_target(host):
    """True for loopback / RFC1918 / link-local / SLIRP-host targets. qos_fetch
    refuses these by default: adding -netdev to every boot gives the guest the
    HOST network namespace via SLIRP NAT (10.0.2.2 == host 127.0.0.1), so an
    unguarded fetch is a blind-SSRF primitive into host-local services."""
    if host.lower() == "localhost":
        return True
    parts = host.split(".")
    if len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        a, b = int(parts[0]), int(parts[1])
        return (a in (0, 10, 127)
                or (a == 172 and 16 <= b <= 31)
                or (a == 192 and b == 168)
                or (a == 169 and b == 254))
    return False


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

        if qseed is not None and not re.match(r"^[0-9A-Fa-f]{1,16}$", str(qseed)):
            self._listen.close()
            self._listen = None
            raise QosError(f"qseed must be 1-16 hex digits (or None): {qseed!r}")
        # `quiet` silences the per-second Timer-tick line and the service-health
        # churn (interrupts.c:380, service.c:457) that would otherwise interleave
        # into a scripted-command window; the one-time [BOOT] milestones and the
        # net self-test's `NET: DHCP lease` line survive (ungated boot_log), so
        # the QSH banner, ISOLATION-VERIFIED settle marker, and net-readiness
        # gate all still work. -netdev gives the guest its NIC so qos_fetch can
        # reach the network (SSRF-guarded host-side in fetch()).
        append = f"qseed={qseed} quiet" if qseed else "quiet"
        cmd = [
            "qemu-system-x86_64", "-kernel", self.kernel,
            "-append", append,
            "-serial", "stdio",                          # COM1: console + qsh
            "-serial", f"tcp:127.0.0.1:{port}",          # COM2: attested bridge
            "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
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

    def await_network(self, timeout_s=12.0):
        """Block until the boot log shows a DHCP lease. net_selftest logs
        `NET: DHCP lease <ip>` via UNGATED boot_log (net.c:999) even under
        `quiet`, so this both serializes a fetch after the lease (the first
        connect must not race DHCP) AND anti-vacuously proves -netdev took
        effect (a NIC-less boot logs `NET: no NIC` instead). Bounded."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if "NET: DHCP lease" in self._log_text():
                return
            if self.proc is not None and self.proc.poll() is not None:
                raise QosDead("VM exited before a DHCP lease")
            time.sleep(0.1)
        raise QosTimeout("no DHCP lease (netdev missing or SLIRP DHCP failed)")

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
        matches; return (body, match). Caller holds _io_lock.

        The echoed command line is dropped before matching: qsh echoes typed
        input verbatim (qsh.c:1392) and it lands in the log BEFORE the real
        output, so a marker must never be allowed to match the agent's own
        input (a recall probe or run args containing a `FIELD:`/`qsh:` line
        form would otherwise spoof the result). Real output always follows the
        CRLF after the echo, so search only past the first newline; markers are
        additionally line-anchored (re.M `^`) by their callers."""
        deadline = time.time() + deadline_s
        while time.time() < deadline:
            text = self._log_text(start)
            nl = text.find("\n")
            body = text[nl + 1:] if nl >= 0 else ""
            for pat in markers:
                m = pat.search(body)
                if m:
                    return body, m
            if self.proc.poll() is not None:
                raise QosDead("VM exited mid-command")
            time.sleep(0.05)
        raise QosTimeout(f"{what}: no completion marker")

    def _qsh(self, line, markers, deadline_s):
        with self._io_lock:
            start = self._send_line(line)
            return self._wait_markers(start, markers, deadline_s, repr(line))

    def _collect(self, cmds, deadline_s, what):
        """Run one or more qsh commands under a single lock hold, then bracket
        their output with a fresh-nonce `echo` sentinel and return the window
        up to that sentinel. The caller extracts by known line prefix (^PS: /
        ^MEM: / ...), so interleaved [BOOT] noise is naturally excluded and the
        fresh nonce can appear in no stored or echoed data. One sentinel round
        trip covers all commands."""
        nonce = "QOS-EOF-" + uuid.uuid4().hex
        sentinel = re.compile(r"^qsh: " + re.escape(nonce) + r"\s*$", re.M)
        with self._io_lock:
            start = self._log_len()
            for c in cmds:
                self._send_line(c)
            self._send_line("echo " + nonce)
            body, _m = self._wait_markers(start, (sentinel,), deadline_s, what)
            return body

    def imprint(self, text, deadline_s=10.0):
        """Kernel holographic field imprint via qsh (same space as a human at
        the prompt). Returns the assigned slot."""
        self._ensure_verified()
        _qsh_text(text, QSH_LINE_MAX - len("imprint "), "imprint text")
        _ok = re.compile(r"^FIELD: imprinted slot (\d+)", re.M)
        _err = re.compile(r"^qsh: imprint(?: failed| : usage|:)", re.M)
        _txt, m = self._qsh("imprint " + text, (_ok, _err), deadline_s)
        if m.re is _err:
            raise QosError("imprint rejected by qsh")
        return {"slot": int(m.group(1)), "identity": self.identity()}

    def recall(self, text, deadline_s=10.0):
        """Kernel holographic field recall via qsh. Returns the winner text."""
        self._ensure_verified()
        _qsh_text(text, QSH_LINE_MAX - len("recall "), "recall probe")
        _win = re.compile(r'^FIELD: winner="(.*?)" slot=(\d+) score=(-?\d+) n=(\d+)', re.M)
        _empty = re.compile(r"^FIELD: recall empty", re.M)
        _err = re.compile(r"^qsh: recall(?: failed|:)", re.M)
        _txt, m = self._qsh("recall " + text, (_win, _empty, _err), deadline_s)
        if m.re is _win:
            return {"winner": m.group(1), "slot": int(m.group(2)),
                    "score": int(m.group(3)), "n": int(m.group(4)),
                    "identity": self.identity()}
        return {"winner": None, "slot": None, "score": None, "n": 0,
                "identity": self.identity()}

    def run(self, program, args="", deadline_s=20.0):
        """Spawn a /bin citizen via qsh with an optional argument string and
        collect its output + exit code. Matches the SPECIFIC spawned pid's exit
        line, so an interleaved boot process exiting can't be mistaken for this
        program's completion. `args` is validated (no control bytes, bounded,
        no marker impersonation) before it is spliced into the run line."""
        self._ensure_verified()
        if not _ALLOWED_RUN.match(program):
            raise QosError(f"program must match /bin/<name>: {program!r}")
        if args:
            _qsh_text(args, QSH_LINE_MAX - len("run ") - len(program) - 1, "run args")
            line = "run " + program + " " + args
        else:
            line = "run " + program
        with self._io_lock:
            start = self._send_line(line)
            _spawned = re.compile(r"^qsh: spawned pid (\d+)", re.M)
            _nostart = re.compile(r"^qsh: run: cannot start", re.M)
            _text, m = self._wait_markers(start, (_spawned, _nostart), deadline_s,
                                          f"run {program}")
            if m.re is _nostart:
                raise QosError(f"qsh could not start {program}")
            pid = m.group(1)
            _done = re.compile(r"^qsh: pid " + pid + r" exited \(code (\d+)\)", re.M)
            _gone = re.compile(r"^qsh: pid " + pid + r" (?:still running|vanished)", re.M)
            text, m = self._wait_markers(start, (_done, _gone), deadline_s,
                                         f"run {program} completion")
            code = int(m.group(1)) if m.re is _done else None
        return {"output": text.strip(), "exit_code": code, "args": args,
                "identity": self.identity()}

    # -- sysinfo / entropy / files / network over scripted qsh --------------
    def sysinfo(self, deadline_s=12.0):
        """One structured snapshot of the machine: uptime, heap/frame memory,
        the live process table, and the RTC date. Collected in a single
        sentinel-bracketed batch; every field is parsed by its own line prefix
        (qsh: uptime / MEM: / PS: / TIME:), so boot noise cannot leak in."""
        self._ensure_verified()
        text = self._collect(["uptime", "free", "ps", "date"], deadline_s, "sysinfo")
        uptime = {}
        mu = re.search(r"^qsh: uptime (\d+) ticks \((\d+) s\)", text, re.M)
        if mu:
            uptime = {"ticks": int(mu.group(1)), "s": int(mu.group(2))}
        mem = {}
        mm = re.search(r"^MEM: heap free=(\d+) bytes, frames free=(\d+)/(\d+)", text, re.M)
        if mm:
            mem = {"heap_free_bytes": int(mm.group(1)),
                   "frames_free": int(mm.group(2)), "frames_total": int(mm.group(3))}
        procs = [{"pid": int(p.group(1)), "name": p.group(2), "state": p.group(3)}
                 for p in re.finditer(r"^PS: (\d+) (\S+) (\w+)", text, re.M)]
        md = re.search(r"^TIME: (.+?)\s*$", text, re.M)
        return {"uptime": uptime, "mem": mem, "processes": procs,
                "date": md.group(1) if md else None, "identity": self.identity()}

    def qrand(self, deadline_s=8.0):
        """Draw 64 bits of quantum-seeded entropy via the qsh `qrand` command
        (SYS_QRAND, capability-gated in the guest). Returns lowercase hex."""
        self._ensure_verified()
        _ok = re.compile(r"^qsh: qrand ([0-9a-f]{16})", re.M)
        _err = re.compile(r"^qsh: qrand denied", re.M)
        _txt, m = self._qsh("qrand", (_ok, _err), deadline_s)
        if m.re is _err:
            raise QosError("qrand denied (EPERM)")
        return {"hex": m.group(1), "bits": 64, "identity": self.identity()}

    def fetch(self, host, port=80, allow_private=False, deadline_s=30.0):
        """Have QuantumOS fetch `http://host:port/` over its own TCP stack and
        report the status line, byte count, and whether the transfer completed.

        HONESTY: this reaches the HOST network namespace via SLIRP NAT
        (10.0.2.2 == the host's 127.0.0.1; DNS and outbound internet are live),
        NOT a sandbox — so loopback / RFC1918 targets are REFUSED by default as
        an SSRF guard (pass allow_private=True to override). The response BODY
        is not returned: qsh's `http` only prints the status line and byte count
        (a body-returning fetch is a filed guest follow-up)."""
        self._ensure_verified()
        if not _FETCH_HOST.match(host):
            raise QosError(f"invalid host {host!r}")
        port = int(port)
        if not 1 <= port <= 65535:
            raise QosError(f"port out of range: {port}")
        if not allow_private and _is_private_target(host):
            raise QosError(
                f"refusing private/loopback target {host!r}: qos_fetch reaches "
                f"the host network via SLIRP (SSRF guard); pass allow_private=True")
        self.await_network()
        _ok = re.compile(r"^qsh: http " + re.escape(host) + r" -> (.+?)\s*$", re.M)
        _err = re.compile(r"^qsh: http: (denied[^\r\n]*|could not resolve host|"
                          r"connect failed|send failed|no response[^\r\n]*)", re.M)
        _bytes = re.compile(r"^qsh: http " + re.escape(host) +
                            r": (\d+) bytes( then connection reset)?", re.M)
        # qsh's `http` prints the host label WITHOUT the port (qsh.c:920).
        line = "http " + host + (" " + str(port) if port != 80 else "")
        with self._io_lock:
            start = self._send_line(line)
            _t, m = self._wait_markers(start, (_ok, _err), deadline_s, f"fetch {host}")
            if m.re is _err:
                return {"ok": False, "error": m.group(1).strip(),
                        "identity": self.identity()}
            status_line = m.group(1).strip()
            _t2, m2 = self._wait_markers(start, (_bytes,), deadline_s,
                                         f"fetch {host} byte count")
        return {"ok": True, "status_line": status_line, "bytes": int(m2.group(1)),
                "complete": m2.group(2) is None, "identity": self.identity()}

    def fs(self, op, path="", text="", deadline_s=12.0):
        """Overlay filesystem write / rm / sync over scripted qsh. read and ls
        are intentionally NOT here: their output is unprefixed (ls rows) or
        agent-controlled (cat content can impersonate the cannot-open line), so
        they need a guest-side machine marker — a filed follow-up. write/rm/sync
        each report a prefixed `qsh:` line that echoed input cannot forge."""
        self._ensure_verified()
        if op == "sync":
            _ok = re.compile(r"^qsh: sync ok \((\d+) files flushed\)", re.M)
            _err = re.compile(r"^qsh: sync failed \(([^)]*)\)", re.M)
            _txt, m = self._qsh("sync", (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"sync failed: {m.group(1)}")
            return {"op": "sync", "flushed": int(m.group(1)), "identity": self.identity()}
        _qsh_path(path)
        if op == "write":
            _qsh_text(text, QSH_LINE_MAX - len("write ") - len(path) - 1, "write text")
            _ok = re.compile(r"^qsh: wrote (\d+) bytes to " + re.escape(path), re.M)
            _err = re.compile(r"^qsh: write: (cannot create[^\r\n]*|usage[^\r\n]*)", re.M)
            _txt, m = self._qsh("write " + path + " " + text, (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"write failed: {m.group(1).strip()}")
            return {"op": "write", "path": path, "bytes": int(m.group(1)),
                    "identity": self.identity()}
        if op == "rm":
            _ok = re.compile(r"^qsh: removed", re.M)
            _err = re.compile(r"^qsh: rm: failed \(err (\d+)\)", re.M)
            _txt, m = self._qsh("rm " + path, (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"rm failed (err {m.group(1)})")
            return {"op": "rm", "path": path, "removed": True, "identity": self.identity()}
        raise QosError(f"unknown fs op {op!r} (write|rm|sync; read/ls deferred)")


def _pdeathsig():
    """Linux: die with the parent so a killed MCP process never orphans QEMU."""
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        PR_SET_PDEATHSIG = 1
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
    except OSError:
        pass
