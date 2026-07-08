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
  6. TAMPER: flip one SIG / ATTEST / PKDIGEST payload byte in the captured
     attestation AND recompute its CRC (so the frame still parses and the
     Lamport digest check — not the checksum — is what rejects it), re-verify
     via the SAME parser, inject into a fresh QosVM, and assert vm.status()
     REFUSES — the exact guard the tools hit.
  7. shutdown -> the QEMU process is gone (proc.poll(), not a raw waitpid).

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps QEMU via
QosVM.shutdown() in the finally + atexit.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qos_bridge
from qos_bridge import (
    QosVM, QosRefused, attestation_from_bytes,
    FrameParser, FRAME_SIG, FRAME_ATTEST, FRAME_PKDIGEST, MAGIC,
)

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
