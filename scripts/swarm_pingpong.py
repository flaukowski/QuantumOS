#!/usr/bin/env python3
"""
QuantumOS swarm-bridge two-way exercise (ghostd phase 4, #51).

Drives the COM2 swarm bridge interactively over a TCP serial backend:
boot QEMU with COM2 as a TCP server, e.g.

    qemu-system-x86_64 -kernel build/x86_64/kernel.elf32 \
        -serial file:/tmp/com1.log \
        -serial tcp:127.0.0.1:5566,server -m 128M -display none -no-reboot

then run this client. It reads and verifies the boot attestation, then sends a
PING and confirms a PONG comes back, and sends a DATA/STATUS request routed to
ghostd and prints the field order parameter it returns. Exit 0 on success.

Stdlib only. The framing lives in qos_bridge.py (epic #99), the single shared
implementation — this file keeps only the interactive PING/STATUS exercise.
"""

import argparse
import socket
import sys
import time

from qos_bridge import (
    FRAME_ATTEST, FRAME_SIG, FRAME_PING, FRAME_PONG, FRAME_DATA,
    SWARM_OP_STATUS, frame, FrameParser,
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--timeout", type=float, default=12.0)
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=2.0)
            break
        except OSError:
            time.sleep(0.2)
    if sock is None:
        print("FAIL: could not connect to COM2 TCP backend")
        return 1
    sock.settimeout(1.0)

    parser = FrameParser()
    have_attest = have_sig = have_ping = have_status = False
    attest_msg = None
    sent_ping = sent_status = False
    r_q16 = None

    while time.time() < deadline and not (have_ping and have_status):
        # Emit the outbound probes once the attestation has streamed in.
        if have_attest and have_sig and not sent_ping:
            sock.sendall(frame(FRAME_PING, b"hi"))
            sent_ping = True
        if have_ping and not sent_status:
            sock.sendall(frame(FRAME_DATA, bytes([SWARM_OP_STATUS])))
            sent_status = True
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        for ftype, payload in parser.feed(chunk):
            if ftype == FRAME_ATTEST:
                attest_msg = payload.decode("ascii", "replace")
                have_attest = True
            elif ftype == FRAME_SIG:
                have_sig = True
            elif ftype == FRAME_PONG:
                have_ping = True
                print(f"PONG received (payload={payload!r})")
            elif ftype == FRAME_DATA and payload and payload[0] == SWARM_OP_STATUS:
                r_q16 = int.from_bytes(payload[1:5], "little")
                have_status = True
                print(f"DATA/STATUS reply: ghostd R={r_q16 / 65536:.4f} "
                      f"live={payload[5] if len(payload) > 5 else '?'}")

    sock.close()

    if attest_msg:
        print(f"attestation seen: {attest_msg!r}")
    if not have_ping:
        print("FAIL: no PONG returned")
        return 1
    if not have_status:
        print("FAIL: no DATA/STATUS reply from ghostd routing")
        return 1
    print("OK: PING->PONG and DATA->ghostd round-trips succeeded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
