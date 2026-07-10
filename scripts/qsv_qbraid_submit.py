#!/usr/bin/env python3
"""Real-QPU submission via qBraid (epic #150 C2) — MAINTAINER-INVOKED ONLY.

This is the credentialed, METERED path: it submits an opaque circuit to a real
quantum processor (Rigetti / IonQ / IQM / QuEra) through the qBraid runtime.
Each submission costs credits and queues, so it is deliberately NOT wired into
any autonomous CI gate and NOT called by the cross-oracle — a human runs it
with credentials and the explicit --confirm flag.

Authority reconciliation (the point of routing real QPUs through the OS): the
in-OS AUDIT_QSUBMIT ledger entry records that a circuit was submitted under a
capability + quota; this script prints the upstream qBraid job id so an auditor
can reconcile the OS-side authority record against the provider's job/billing
record — closing the "who spent this quantum credit, under what grant" loop.

Prereqs: ~/.qbraid/qbraidrc with an api-key; `pip install qbraid`. Usage:
    python scripts/qsv_qbraid_submit.py --machine <id> [--shots 1000] --confirm
Without --confirm it prints the plan and exits WITHOUT contacting qBraid or
spending anything.
"""
import argparse
import sys

import qpu_circuit as qc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--machine", required=True, help="qBraid device id (real QPU)")
    ap.add_argument("--shots", type=int, default=1000)
    ap.add_argument("--circuit", default="bell", choices=["bell", "ghz3"],
                    help="which canonical circuit to submit")
    ap.add_argument("--confirm", action="store_true",
                    help="REQUIRED to actually submit (spends credits); without it, dry-run")
    args = ap.parse_args()

    circuit = qc.bell() if args.circuit == "bell" else qc.ghz(3)
    n, _probe, ops = qc.parse(circuit)
    print(f"plan: submit '{args.circuit}' ({n} qubits, {len(ops)} ops) to qBraid "
          f"machine '{args.machine}' for {args.shots} shots")

    if not args.confirm:
        print("DRY RUN — pass --confirm to submit (this spends qBraid credits). "
              "Nothing contacted, nothing spent.")
        return 0

    # Real submission (guarded behind --confirm). Translate the opaque circuit to
    # a PennyLane tape and run it on the qBraid device via the PennyLane plugin,
    # OR submit through qbraid.runtime directly — whichever the installed qBraid
    # exposes. Kept import-local so the module loads without qbraid present.
    import pennylane as qml
    import qsv_gateway  # reuse the wire->PennyLane translation

    dev = qml.device("qbraid.qubit", wires=n, device=args.machine, shots=args.shots)
    _n, _p, ops = qc.parse(circuit)

    @qml.qnode(dev)
    def run():
        for opcode, a, b in ops:
            if opcode == qc.QC_OP_H:
                qml.Hadamard(a)
            elif opcode == qc.QC_OP_X:
                qml.PauliX(a)
            elif opcode == qc.QC_OP_CNOT:
                qml.CNOT([a, b])
            else:
                raise NotImplementedError(f"opcode {opcode} not mapped for the QPU path")
        return qml.counts(wires=list(range(n)))

    counts = run()
    print(f"counts: {dict(counts)}")
    job_id = getattr(getattr(dev, "tracker", None), "latest", None) or "<see qBraid dashboard>"
    print(f"qBraid job id: {job_id}")
    print("RECONCILE: match this job id against the OS AUDIT_QSUBMIT ledger entry "
          "for the same submission (backend id + upstream job id) — the authority "
          "loop (who submitted, under what grant/quota) closes here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
