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
  3. The Lamport signature is valid: each revealed preimage hashes to the
     public-key entry for its message-digest bit, and the reconstructed public
     key hashes to the committed public-key digest.
  4. The attested qseed matches the one expected on the kernel cmdline
     (compared numerically; `none` when the boot carried no qseed).

Exit 0 if all pass, 1 otherwise. Standard library only (hashlib).

Frame layout (little-endian):
    0xA5 | type:u8 | len:u16 | payload[len] | crc8
Lamport parameters and the exact key/signature construction are documented in
user/swarm.h and mirrored here.
"""

import argparse
import hashlib
import sys

MAGIC = 0xA5
FRAME_HANDSHAKE = 0x01
FRAME_PKDIGEST = 0x10
FRAME_ATTEST = 0x11
FRAME_SIG = 0x12

LAMPORT_BITS = 256
HASH_LEN = 32
SIG_ELEM = 64            # revealed preimage (32) + complementary pk hash (32)
SIG_LEN = LAMPORT_BITS * SIG_ELEM   # 16384


def crc8(data: bytes) -> int:
    """CRC-8/CCITT (poly 0x07, init 0x00, MSB-first) — matches swarm_crc8()."""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def parse_frames(blob: bytes):
    """Yield (type, payload) for every complete, CRC-valid frame; count bad CRCs."""
    frames = []
    bad_crc = 0
    i = 0
    n = len(blob)
    while i < n:
        if blob[i] != MAGIC:
            i += 1
            continue
        if i + 4 > n:
            break
        ftype = blob[i + 1]
        length = blob[i + 2] | (blob[i + 3] << 8)
        end = i + 4 + length
        if end + 1 > n:
            break
        payload = blob[i + 4:end]
        crc = blob[end]
        if crc8(blob[i + 1:end]) == crc:
            frames.append((ftype, payload))
            i = end + 1
        else:
            bad_crc += 1
            i += 1   # resync past this magic
    return frames, bad_crc


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def verify_lamport(message: bytes, pkdigest: bytes, signature: bytes) -> bool:
    """Rebuild the public key from the signature and check it against the
    committed digest, and that each preimage hashes to its pk entry."""
    if len(signature) != SIG_LEN:
        print(f"FAIL: signature is {len(signature)} bytes, expected {SIG_LEN}")
        return False
    if len(pkdigest) != HASH_LEN:
        print(f"FAIL: pk digest is {len(pkdigest)} bytes, expected {HASH_LEN}")
        return False

    md = sha256(message)
    pk_stream = bytearray()
    for i in range(LAMPORT_BITS):
        bit = (md[i >> 3] >> (i & 7)) & 1
        elem = signature[i * SIG_ELEM:(i + 1) * SIG_ELEM]
        preimage = elem[:HASH_LEN]
        comp = elem[HASH_LEN:]
        pk_bit = sha256(preimage)          # pk[i][bit]
        pk = [None, None]
        pk[bit] = pk_bit
        pk[1 - bit] = comp                 # pk[i][1-bit], from the signature
        pk_stream += pk[0]
        pk_stream += pk[1]

    recomputed = sha256(bytes(pk_stream))
    if recomputed != pkdigest:
        print("FAIL: reconstructed public key does not match committed digest")
        return False
    return True


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


def expected_qseed(arg: str):
    return None if arg.lower() == "none" else int(arg, 16)


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
