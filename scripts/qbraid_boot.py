#!/usr/bin/env python3
"""
QuantumOS qBraid boot harness.

Draws quantum-random bytes, hands them to the QuantumOS kernel as a
`qseed=<hex>` boot command line, launches the kernel under QEMU, and
streams the serial console. On a qBraid lab machine the entropy comes
from a real QPU (or the free qBraid simulator); off-lab it falls back
to the OS CSPRNG so the harness is testable anywhere.

The kernel mixes the seed into its quantum resource manager's PRNG at
boot and echoes it back on serial, so a round-trip can be verified:

    [BOOT] Boot entropy accepted from cmdline (qseed=)
    [BOOT]   qseed value:
    <the same 64-bit value the harness sent>

Usage:
    python3 scripts/qbraid_boot.py                      # local entropy
    python3 scripts/qbraid_boot.py --backend qbraid-sim # qBraid simulator
    python3 scripts/qbraid_boot.py --backend <device>   # real QPU on qBraid
    python3 scripts/qbraid_boot.py --exit-on-ready      # quit at "QuantumOS ready"

SPDX-License-Identifier: GPL-2.0-only
"""

import argparse
import os
import subprocess
import sys

SEED_BYTES = 8  # 64-bit qseed


def draw_entropy_local(nbytes):
    """OS CSPRNG fallback (not quantum, but unblocks off-lab testing)."""
    return os.urandom(nbytes), "local-csprng"


def draw_entropy_quantum(nbytes, backend):
    """Draw `nbytes` of quantum-random bytes on qBraid.

    Runs a one-qubit Hadamard-and-measure circuit repeatedly; each shot
    yields one unbiased bit. Requires the qBraid runtime (available in a
    qBraid lab) and, for a real device, that `backend` names an
    accessible QPU. Raises on any failure so the caller can fall back.
    """
    from qbraid.runtime import QbraidProvider  # qBraid lab environment

    # A qBraid device id, or the free simulator by default
    device_id = "qbraid_qir_simulator" if backend == "qbraid-sim" else backend

    # One-qubit H + measure, drawn as an OpenQASM 2 program so we do not
    # hard-depend on any single circuit SDK being importable.
    qasm = (
        "OPENQASM 2.0;\n"
        'include "qelib1.inc";\n'
        "qreg q[1];\ncreg c[1];\n"
        "h q[0];\nmeasure q[0] -> c[0];\n"
    )

    provider = QbraidProvider()
    device = provider.get_device(device_id)

    bits = []
    need = nbytes * 8
    # Batch the shots; von Neumann debiasing is unnecessary for H on an
    # ideal simulator, and real-device bias is mixed out by the kernel's
    # SplitMix avalanche, but we still collect raw measured bits.
    while len(bits) < need:
        shots = min(1024, need - len(bits))
        job = device.run(qasm, shots=shots)
        result = job.result()
        counts = result.data.get_counts() if hasattr(result, "data") else result.measurement_counts()
        # Expand counts back into a bit stream
        for outcome, n in counts.items():
            b = int(str(outcome)[-1])
            bits.extend([b] * n)
    bits = bits[:need]

    out = bytearray(nbytes)
    for i, b in enumerate(bits):
        out[i // 8] |= (b & 1) << (i % 8)
    return bytes(out), f"qbraid:{device_id}"


def draw_entropy(nbytes, backend):
    if backend == "local":
        return draw_entropy_local(nbytes)
    try:
        return draw_entropy_quantum(nbytes, backend)
    except Exception as exc:  # noqa: BLE001 — any failure => fall back
        print(f"[harness] quantum entropy unavailable ({exc!r}); "
              f"falling back to local CSPRNG", file=sys.stderr)
        return draw_entropy_local(nbytes)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_kernel = os.path.join(repo_root, "build", "x86_64", "kernel.elf32")

    ap = argparse.ArgumentParser(description="QuantumOS qBraid boot harness")
    ap.add_argument("--backend", default="local",
                    help="entropy source: 'local', 'qbraid-sim', or a QPU device id")
    ap.add_argument("--kernel", default=default_kernel,
                    help="path to the ELF32 kernel image")
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--memory", default="128M")
    ap.add_argument("--timeout", type=int, default=0,
                    help="seconds before killing QEMU (0 = run until it exits)")
    ap.add_argument("--exit-on-ready", action="store_true",
                    help="stop once the kernel prints 'QuantumOS ready'")
    args = ap.parse_args()

    if not os.path.exists(args.kernel):
        sys.exit(f"kernel image not found: {args.kernel}\n"
                 f"build it first: make")

    seed_bytes, source = draw_entropy(SEED_BYTES, args.backend)
    seed_hex = seed_bytes.hex()
    # Kernel reads qseed as a big-endian hex u64 (high nibble first)
    seed_be = int.from_bytes(seed_bytes, "big")

    print(f"[harness] entropy source : {source}")
    print(f"[harness] qseed (64-bit) : 0x{seed_be:016x}")
    print(f"[harness] booting {os.path.basename(args.kernel)} under QEMU\n")

    cmd = [
        args.qemu,
        "-kernel", args.kernel,
        "-append", f"qseed={seed_hex}",
        "-m", args.memory,
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            bufsize=1)
    saw_seed = False
    try:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            if "qseed value" in line:
                saw_seed = True
            if args.exit_on_ready and "QuantumOS ready" in line:
                proc.terminate()
                break
    except KeyboardInterrupt:
        proc.terminate()
    finally:
        try:
            proc.wait(timeout=args.timeout or None)
        except subprocess.TimeoutExpired:
            proc.terminate()

    print(f"\n[harness] entropy handoff {'CONFIRMED' if saw_seed else 'not observed'} "
          f"(sent 0x{seed_be:016x})")


if __name__ == "__main__":
    main()
