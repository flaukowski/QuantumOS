#!/usr/bin/env python3
"""
QuantumOS MCP lifecycle gate (epic #99). Stdlib-only: it drives qos_bridge
directly (the exact code path the FastMCP tools delegate to) and NEVER imports
the `mcp` package, so it runs in the integration CI job that has no pip.

Full lifecycle against ONE VM, every step anti-vacuous:
  1. boot qseed=DEADBEEFCAFEBABE -> attestation verified AND attested qseed
     matches the request (numeric).
  2. ghostd STATUS over COM2 -> live >= 3 (the boot self-test's field is alive
     in the bridge path; asserting ==1 would be red, ghost_test imprints 3).
  3+4. imprint TWO distinct phrases at the kernel field, recall a typo'd copy
     of each -> each returns its OWN phrase and the two winners DIFFER
     (discrimination — best-of-one cannot satisfy it). (The field is
     content-addressable: every probe lands on its nearest attractor, so
     there is no "returns nothing" case to assert.)
  5. run /bin/hello -> exit code 42, hello's unique deterministic signature
     (qsh reports it from SYS_WAITPID; the number cannot come from echoing the
     typed 'run /bin/hello'). Retried a few times: an early boot spawn that
     reuses a just-freed transient pid slot is intermittently stillborn — a
     pre-existing kernel early-spawn race the CI shell gate dodges by running
     hello deep into boot; here we simply retry until the real run lands.
  5b-5h. The Phase A toolbox (epic #125), each anti-vacuous: run /bin/args
     'alpha beta' -> exit 3 + un-echoable `ARGS: argv[2]=beta`; sysinfo ->
     advancing uptime, positive heap, a process table with qsh RUNNING and only
     known states, an RTC date; qrand -> two distinct 16-hex draws; fs ->
     write/rm roundtrip on the overlay with an honest no-disk sync and a
     rm-missing error; fetch -> a hermetic in-process http.server (guest reaches
     it via SLIRP 10.0.2.2, bytes >= the 4096-byte body, server hit >= 1), a
     dead-port refusal, and the SSRF guard refusing 10.0.2.2 by default;
     injection -> a \n payload refused on ALL SIX line-builders (imprint,
     recall, run-args, fs-text, fs-path, fetch-host) with a canary that must
     survive, plus the security-critical qsh line-length budget (exact ok,
     +1 refused, not truncated).
  6. TAMPER: flip one SIG / ATTEST / PKDIGEST payload byte in the captured
     attestation AND recompute its CRC (so the frame still parses and the
     Lamport digest check — not the checksum — is what rejects it), re-verify
     via the SAME parser, inject into a fresh QosVM, and assert vm.status()
     REFUSES — the exact guard the tools hit.
  7. shutdown -> the QEMU process is gone (proc.poll(), not a raw waitpid).

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps QEMU via
QosVM.shutdown() in the finally + atexit.
"""

import http.server
import sys
import os
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qos_bridge
from qos_bridge import (
    QosVM, QosError, QosRefused, attestation_from_bytes,
    QSH_LINE_MAX, FrameParser, FRAME_SIG, FRAME_ATTEST, FRAME_PKDIGEST, MAGIC,
)

_PROC_STATES = {"UNUSED", "CREATED", "READY", "RUNNING", "BLOCKED",
                "TERMINATED", "ZOMBIE"}
_CANARY = "/notes/canary.txt"

QSEED = "DEADBEEFCAFEBABE"
PHRASE_A = "the cat sat on the mat"
PHRASE_A_NOISY = "the cxt sat on thx mat"
PHRASE_B = "quantum ghosts dream in phase"
PHRASE_B_NOISY = "quantum ghxsts drexm in phase"


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _first_frame_span(blob, ftype):
    """(frame_start, payload_start, crc_pos) of the first frame of `ftype`."""
    i = 0
    n = len(blob)
    while i < n:
        if blob[i] != MAGIC:
            i += 1
            continue
        if i + 4 > n:
            break
        length = blob[i + 2] | (blob[i + 3] << 8)
        if length > qos_bridge.SWARM_MAX_PAYLOAD:
            i += 1
            continue
        end = i + 4 + length
        if end + 1 > n:
            break
        if qos_bridge.crc8(blob[i + 1:end]) == blob[end]:
            if blob[i + 1] == ftype:
                return i, i + 4, end
            i = end + 1
        else:
            i += 1
    return None


def _assert_tamper_refuses(raw, ftype, label):
    span = _first_frame_span(raw, ftype)
    if span is None:
        _fail(f"tamper setup: no {label} frame in the capture")
    fstart, pstart, crc_pos = span
    mutated = bytearray(raw)
    mutated[pstart] ^= 0x01                                 # flip one payload bit
    mutated[crc_pos] = qos_bridge.crc8(mutated[fstart + 1:crc_pos])  # fix CRC so the
    # frame still parses — this forces the Lamport digest check (not the CRC) to
    # be what rejects the tamper.
    att = attestation_from_bytes(bytes(mutated), QSEED)
    if att.verified:
        _fail(f"tamper ({label}): a flipped byte still verified")
    if "CRC" in att.reason:
        _fail(f"tamper ({label}): rejected at CRC, not the signature: {att.reason}")
    vm = QosVM()
    vm.attestation = att           # inject; no boot, no socket
    try:
        vm.status()
    except QosRefused as exc:
        print(f"OK: tamper ({label}) -> refused by crypto: {att.reason}")
        return
    _fail(f"tamper ({label}): status() did not refuse")


class _FixedBody(http.server.BaseHTTPRequestHandler):
    """A hermetic in-process server: 200 + a known 4096-byte body, counting
    hits, so the fetch gate proves the guest's TCP stack reached OUR server end
    to end (bytes >= body length AND a request actually landed) rather than
    passing on a bare status line. No pip, no egress — loopback via SLIRP."""
    body = b"Q" * 4096
    hits = 0

    def do_GET(self):
        _FixedBody.hits += 1
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(_FixedBody.body)))
        self.end_headers()
        self.wfile.write(_FixedBody.body)

    def log_message(self, *_a):
        pass


def _exercise_run_args(vm):
    """run /bin/args with an argument vector -> exit code is argc (un-echoable:
    the kernel splits the line into argv and /bin/args prints it from ring 3).
    Retries past the pid-recycle stillborn-spawn race, like /bin/hello."""
    out = None
    for _ in range(5):
        out = vm.run("/bin/args", args="alpha beta")
        if out["exit_code"] == 3:      # argv0 + 2 args
            break
    if out["exit_code"] != 3:
        _fail(f"/bin/args argc != 3 (last exit_code={out['exit_code']})")
    if "ARGS: argv[2]=beta" not in out["output"]:
        _fail(f"/bin/args did not echo argv[2]=beta: {out['output']!r}")
    print("OK: run /bin/args 'alpha beta' -> exit 3, argv[2]=beta (un-echoable)")


def _exercise_sysinfo(vm):
    info = vm.sysinfo()
    if info["uptime"].get("ticks", 0) <= 0:
        _fail(f"sysinfo uptime not advancing: {info['uptime']}")
    if info["mem"].get("heap_free_bytes", 0) <= 0:
        _fail(f"sysinfo heap_free not positive: {info['mem']}")
    procs = info["processes"]
    if len(procs) < 3:
        _fail(f"sysinfo process table too small: {len(procs)}")
    if not any(p["name"] == "qsh" and p["state"] == "RUNNING" for p in procs):
        _fail("sysinfo did not show qsh RUNNING (the shell driving this)")
    bad = [p for p in procs if p["state"] not in _PROC_STATES]
    if bad:
        _fail(f"sysinfo unknown process state(s): {bad}")
    if not (info["date"] or "").startswith("20"):
        _fail(f"sysinfo date not an RTC timestamp: {info['date']!r}")
    print(f"OK: sysinfo uptime={info['uptime']['s']}s heap_free="
          f"{info['mem']['heap_free_bytes']} procs={len(procs)} date={info['date']}")


def _exercise_qrand(vm):
    a = vm.qrand()["hex"]
    b = vm.qrand()["hex"]
    if len(a) != 16 or any(c not in "0123456789abcdef" for c in a):
        _fail(f"qrand not 16 lowercase hex: {a!r}")
    if a == b:
        _fail("two qrand draws were identical (entropy dead)")
    print(f"OK: qrand drew {a} then {b} (distinct)")


def _exercise_fs(vm):
    w = vm.fs("write", path="/notes/probe.txt", text="hello-quantum-42")
    if w["bytes"] < len("hello-quantum-42"):
        _fail(f"fs write byte count too low: {w}")
    try:
        s = vm.fs("sync")
        print(f"OK: fs sync flushed {s['flushed']} file(s) (disk attached)")
    except QosError as exc:
        if "no disk" not in str(exc):
            _fail(f"fs sync failed for an unexpected reason: {exc}")
        print(f"OK: fs sync honestly reported no disk ({exc})")
    if not vm.fs("rm", path="/notes/probe.txt")["removed"]:
        _fail("fs rm did not report removal")
    try:
        vm.fs("rm", path="/notes/probe.txt")
        _fail("fs rm of a now-missing file did not error")
    except QosError:
        pass
    print("OK: fs write -> (sync) -> rm roundtrip; rm-missing errored")


def _exercise_fetch(vm):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FixedBody)
    port = srv.server_address[1]
    _FixedBody.hits = 0
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        res = vm.fetch("10.0.2.2", port=port, allow_private=True)
        if not res.get("ok"):
            _fail(f"hermetic fetch failed: {res}")
        if "200" not in res["status_line"]:
            _fail(f"fetch status line not 200: {res['status_line']!r}")
        if res["bytes"] < len(_FixedBody.body):
            _fail(f"fetch byte count {res['bytes']} < body {len(_FixedBody.body)}")
        if not res["complete"]:
            _fail(f"fetch reported an incomplete transfer: {res}")
        if _FixedBody.hits < 1:
            _fail("fetch never reached the in-process server (hits=0)")
        print(f"OK: fetch 10.0.2.2:{port} -> {res['status_line']} "
              f"{res['bytes']} bytes, server saw {_FixedBody.hits} request(s)")
    finally:
        srv.shutdown()
        srv.server_close()

    # Negative: the SAME port is now dead (server closed) -> a genuine failure,
    # not a flake. Run only after the positive proved NIC + lease, so the error
    # discriminates. libslirp may surface a refused connect as either line.
    neg = vm.fetch("10.0.2.2", port=port, allow_private=True)
    if neg.get("ok"):
        _fail(f"fetch of a dead port unexpectedly succeeded: {neg}")
    if neg["error"] not in ("connect failed", "no response (timed out)"):
        _fail(f"dead-port fetch error not in the expected set: {neg['error']!r}")
    print(f"OK: fetch of the closed port refused ({neg['error']})")

    # SSRF policy: a loopback/SLIRP-host target is refused BEFORE the network
    # unless allow_private is set (default False).
    try:
        vm.fetch("10.0.2.2", port=port)
        _fail("SSRF guard did not refuse 10.0.2.2 by default")
    except QosError as exc:
        if "private" not in str(exc).lower():
            _fail(f"SSRF refusal for the wrong reason: {exc}")
    print("OK: SSRF guard refused 10.0.2.2 by default (allow_private required)")


def _exercise_injection(vm):
    """Every method that splices agent input into a qsh line must refuse a
    control byte, and a pre-written canary must survive byte-identical. The
    read-free survival proof: rm the canary at the end — if it still exists the
    rm succeeds, if any injection had run `rm <canary>` the final rm errors."""
    vm.fs("write", path=_CANARY, text="canary-INTACT")
    payload = "x\nrm " + _CANARY                      # a newline => second command
    builders = [
        ("imprint", lambda: vm.imprint(payload)),
        ("recall", lambda: vm.recall(payload)),
        ("run-args", lambda: vm.run("/bin/args", args=payload)),
        ("fs-text", lambda: vm.fs("write", path="/notes/x.txt", text=payload)),
        ("fs-path", lambda: vm.fs("write", path="/notes/x\nrm " + _CANARY, text="y")),
        ("fetch-host", lambda: vm.fetch("h\nrm " + _CANARY, allow_private=True)),
    ]
    for label, fn in builders:
        try:
            fn()
            _fail(f"injection via {label} was NOT refused")
        except QosError:
            pass
    survived = vm.fs("rm", path=_CANARY)
    if not survived.get("removed"):
        _fail("canary was deleted — an injection leaked a second command")
    print("OK: \\n-injection refused on all 6 line-builders; canary survived")

    # The line budget is security-critical (over-budget => qsh runs a
    # truncated, different command): exactly-budget succeeds end to end,
    # budget+1 is refused (not silently truncated). Exercised via `write`,
    # whose argument reaches the ramfs verbatim (the field's 64-byte
    # FIELD_PAT_MAX would otherwise mask the line budget).
    bud_path = "/notes/bud.txt"
    budget = QSH_LINE_MAX - len("write ") - len(bud_path) - 1
    w = vm.fs("write", path=bud_path, text="L" * budget)   # exactly fills the line
    if w["bytes"] < budget:
        _fail(f"exact-budget write stored too few bytes: {w}")
    try:
        vm.fs("write", path=bud_path, text="L" * (budget + 1))
        _fail("over-budget write was not refused")
    except QosError:
        pass
    vm.fs("rm", path=bud_path)
    print(f"OK: qsh line budget enforced (write {budget} ok, {budget + 1} refused)")


def main():
    # QOS_KERNEL lets CI point at the downloaded artifact (its build/ path is
    # unpredictable); locally QosVM finds the standard build output itself.
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    try:
        # 1. boot + verified identity + qseed binding.
        ident = vm.boot(qseed=QSEED, timeout=45)
        if not ident.get("verified"):
            _fail(f"boot attestation not verified: {ident.get('reason')}")
        if ident.get("qseed") != QSEED:
            _fail(f"attested qseed {ident.get('qseed')} != {QSEED}")
        print(f"OK: booted, attestation verified, qseed={ident['qseed']} "
              f"ticks={ident['ticks']} nonce={ident['boot_nonce']}")

        # 2. ghostd STATUS over COM2 (distinct ghostd Hopfield field).
        st = vm.status()
        if st["live"] < 3:
            _fail(f"ghostd live count {st['live']} < 3 (boot self-test field missing)")
        print(f"OK: ghostd STATUS live={st['live']} R={st['r']:.4f}")

        # 3. imprint two distinct phrases (kernel holographic field via qsh).
        sa = vm.imprint(PHRASE_A)["slot"]
        sb = vm.imprint(PHRASE_B)["slot"]
        if sa == sb:
            _fail(f"two imprints collided on slot {sa}")
        print(f"OK: imprinted A->slot {sa}, B->slot {sb}")

        # 4. recall a typo'd copy of each -> its own phrase; winners differ.
        wa = vm.recall(PHRASE_A_NOISY)["winner"]
        wb = vm.recall(PHRASE_B_NOISY)["winner"]
        if wa != PHRASE_A:
            _fail(f"noisy-A recall returned {wa!r}, expected {PHRASE_A!r}")
        if wb != PHRASE_B:
            _fail(f"noisy-B recall returned {wb!r}, expected {PHRASE_B!r}")
        if wa == wb:
            _fail("both recalls returned the same winner (no discrimination)")
        print(f"OK: recall discriminated noisy-A->{wa!r} noisy-B->{wb!r}")

        # 5. run a citizen off the initrd. Exit 42 is hello's unique
        # deterministic signature (un-echoable). Retry past the early-spawn
        # stillborn race (exit 0, program never ran).
        code = None
        for _ in range(5):
            run = vm.run("/bin/hello")
            code = run["exit_code"]
            if code == 42:
                break
        if code != 42:
            _fail(f"/bin/hello never returned exit code 42 (last={code})")
        print("OK: ran /bin/hello (exit code 42 — its unique signature)")

        # 5b-5h. The Phase A toolbox (epic #125): run-with-args, sysinfo, qrand,
        # overlay fs, the OS's own network fetch (hermetic + negative + SSRF),
        # and the injection/length guards that keep the qsh line protocol safe.
        _exercise_run_args(vm)
        _exercise_sysinfo(vm)
        _exercise_qrand(vm)
        _exercise_fs(vm)
        _exercise_fetch(vm)
        _exercise_injection(vm)

        # 6. tamper -> refusal, on the SAME code path the tools use.
        raw = vm.attestation.raw
        _assert_tamper_refuses(raw, FRAME_SIG, "SIG")
        _assert_tamper_refuses(raw, FRAME_ATTEST, "ATTEST")
        _assert_tamper_refuses(raw, FRAME_PKDIGEST, "PKDIGEST")

        # 7. shutdown -> QEMU gone.
        proc = vm.proc
        vm.shutdown()
        if proc is not None and proc.poll() is None:
            _fail("QEMU still running after shutdown")
        print("OK: shutdown reaped QEMU")

        # stdlib-only guarantee: neither we nor qos_bridge pulled in `mcp`.
        if "mcp" in sys.modules:
            _fail("the `mcp` package leaked into the stdlib-only test")
        print("OK: no `mcp` import (integration CI stays pip-free)")

        print("=== MCP lifecycle gate PASSED ===")
        return 0
    finally:
        vm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
