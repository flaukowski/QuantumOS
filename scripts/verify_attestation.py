#!/usr/bin/env python3
"""
QuantumOS boot-attestation verifier (ghostd phase 4, #51; epic #47).

Reads the COM2 swarm-bridge byte stream QuantumOS writes at boot (captured
with QEMU's `-serial file:<path>`), parses the CRC8-framed frames, and
verifies the Lamport-signed boot attestation emitted by the ring-3 swarm_svc.

Checks, in order:
  1. Every frame's CRC8 is valid (CRC-8/CCITT, poly 0x07, init 0x00).
  2. A public-key-digest frame, an attestation frame, and a full 16384-byte
     signature were emitted.
  3. The Lamport signature is valid: the reconstructed public key hashes to
     the committed public-key digest.
  4. The attested qseed matches the one expected on the kernel cmdline
     (compared numerically; `none` when the boot carried no qseed).

Exit 0 if all pass, 1 otherwise. Standard library only.

The framing + Lamport verification live in qos_bridge.py (epic #99), which is
the single implementation shared with the MCP server and swarm_pingpong.py —
this file keeps only the CLI and the pass/fail reporting.
"""

import argparse
import sys

from qos_bridge import (
    FRAME_HANDSHAKE, FRAME_PKDIGEST, FRAME_ATTEST, FRAME_SIG,
    parse_frames, verify_lamport, parse_attestation, expected_qseed,
)


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify a QuantumOS COM2 boot attestation")
    ap.add_argument("logfile", help="COM2 byte stream captured via QEMU -serial file:")
    ap.add_argument("--qseed", default="none",
                    help="qseed expected on the kernel cmdline (hex), or 'none'")
    args = ap.parse_args()

    with open(args.logfile, "rb") as f:
        blob = f.read()

    frames, bad_crc = parse_frames(blob)
    print(f"parsed {len(frames)} valid frames from {len(blob)} bytes "
          f"({bad_crc} CRC failures)")
    if bad_crc:
        print("FAIL: frames with invalid CRC8 present")
        return 1

    pkdigest = None
    attest_msg = None
    sig = bytearray()
    handshake = False
    for ftype, payload in frames:
        if ftype == FRAME_HANDSHAKE:
            handshake = True
        elif ftype == FRAME_PKDIGEST:
            pkdigest = bytes(payload)
        elif ftype == FRAME_ATTEST:
            attest_msg = payload.decode("ascii", errors="replace")
        elif ftype == FRAME_SIG:
            sig += payload

    if not handshake:
        print("WARN: no HANDSHAKE frame (non-fatal)")
    if pkdigest is None or attest_msg is None or not sig:
        print("FAIL: missing pk-digest, attestation, or signature frame(s)")
        return 1

    print(f"attestation: {attest_msg!r}")

    try:
        qseed_val, ticks = parse_attestation(attest_msg)
    except ValueError as exc:
        print(f"FAIL: {exc}")
        return 1

    if not verify_lamport(attest_msg.encode("ascii"), pkdigest, bytes(sig)):
        print("FAIL: reconstructed public key does not match committed digest")
        return 1
    print("OK: Lamport signature valid (public key matches committed digest)")

    want = expected_qseed(args.qseed)
    if qseed_val != want:
        want_s = "none" if want is None else f"{want:X}"
        got_s = "none" if qseed_val is None else f"{qseed_val:X}"
        print(f"FAIL: attested qseed {got_s} != cmdline qseed {want_s}")
        return 1
    print(f"OK: attested qseed matches cmdline "
          f"({'none' if want is None else format(want, 'X')})")

    print(f"OK: attestation verified (ticks={ticks})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
