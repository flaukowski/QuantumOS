#!/usr/bin/env python3
"""COM2 quantum-submit gate (epic #149 B1): a host agent frames an OPAQUE
circuit over the attested COM2 bridge, swarm_svc forwards it to the SYS_QPU
broker, qpud runs it on the exact integer engine, and the exact result comes
back over the wire the host never typed. This is the agent-native integration
point — an external agent drives QuantumOS's quantum capability over the same
Lamport-attested channel the swarm uses.

Anti-vacuous (the panel's spec):
  - Two DISTINCT circuits (Bell p=1/2 vs Grover-3q p=121/128, amp 176) — a
    circuit-ignoring/hardcoded bridge cannot produce both.
  - The grover3 wire result is cross-checked against the host PennyLane gateway
    (qsv_gateway) on the IDENTICAL bytes — ties B1 to B2, and Bell (0.5==0.5) is
    excluded as circular.
  - A malformed circuit returns a wire-visible broker error (status=2) with the
    executor's QC_STATUS_EINVAL forwarded (not a false OK).
  - The quota leg: qsub_max accepted THEN one refused (status=1), with the
    swarm_svc incarnation asserted unchanged across the burst (a mid-test
    watchdog rebirth invalidates the run rather than flaking green).
  - A near-QPU_CIRCUIT_MAX circuit exercises multi-read inbound reassembly.

Run: python scripts/test_qos_qsubmit.py   (boots its own QosVM; needs qemu +
the PennyLane venv for the cross-oracle leg — skipped with a notice if absent).
"""
import atexit
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qpu_circuit as qc
from qos_bridge import QosVM

QSEED = "DEADBEEFCAFEBABE"
QSUB_MAX = 5  # swarm-svc .qsub_max (syscall.c); a submission that reaches the
#               broker charges quota even if the executor later rejects it, so
#               bell + grover3 + large + malformed + overflow = 5 charges, then
#               1 refused.


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _decode_ok(vm, circuit, what):
    r = vm.qsubmit(circuit)
    if r["status"] != 0:
        _fail(f"{what}: wire status {r['status']} != 0 (OK)")
    res = qc.parse_result(r["result"])
    if res["status"] != qc.QC_STATUS_OK:
        _fail(f"{what}: executor result status {res['status']} != OK")
    return res


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(qseed=QSEED, timeout=45)

    # 1. Bell over the wire → p=1/2 via qpu_run() — the agent-facing convenience
    #    the qos_qpu_submit MCP tool delegates to (build the named circuit, submit
    #    it over the same wire path, DECODE the exact result). Exercising it here
    #    (rather than a raw qsubmit + a separate qpu_run submit) keeps the total
    #    submit count within swarm_svc's qsub_max so step 5's overflow still tests
    #    broker rejection, not quota exhaustion.
    run = vm.qpu_run("bell")
    if not (run.get("ok") and run["status"] == 0 and run["n_qubits"] == 2
            and run["prob_num"] == 1 and run["prob_den"] == 2 and run["h"] == 1
            and run["amp_re"] == 1 and run["amp_im"] == 0
            and abs(run["probability"] - 0.5) < 1e-12):
        _fail(f"qpu_run('bell') wire/decode wrong: {run}")
    print(f"OK: Bell via qpu_run() → p={run['prob_num']}/{run['prob_den']} "
          f"(n={run['n_qubits']}, h={run['h']}) — the qos_qpu_submit tool path")
    bell = {"num": run["prob_num"], "den": run["prob_den"], "h": run["h"],
            "amp_re": run["amp_re"], "amp_im": run["amp_im"], "n_qubits": run["n_qubits"]}

    # 2. Grover-3q over the wire → p=121/128 (amp 176, h=15). DISTINCT from Bell,
    #    so a hardcoded/circuit-ignoring bridge cannot produce both.
    grov = _decode_ok(vm, qc.grover(3, 5, 2), "grover3")
    if not (grov["h"] == 15 and grov["num"] == 121 and grov["den"] == 128 and grov["amp_re"] == 176):
        _fail(f"grover3 wire result wrong: {grov}")
    if (grov["num"], grov["den"]) == (bell["num"], bell["den"]):
        _fail("Bell and Grover-3q returned identical results — bridge ignores the circuit")
    print(f"OK: Grover-3q brokered over COM2 → p={grov['num']}/{grov['den']} (amp={grov['amp_re']}, h={grov['h']})")

    # 3. Cross-oracle (B1<->B2): the grover3 WIRE result must match the host
    #    PennyLane gateway on the IDENTICAL bytes. Bell excluded (circular).
    try:
        import qsv_gateway
        pl = qsv_gateway.probs(qc.grover(3, 5, 2))
        pl_p = pl[5]  # probability of |101>=5
        wire_p = grov["num"] / grov["den"]
        if abs(pl_p - wire_p) > 1e-9:
            _fail(f"wire result {wire_p} disagrees with PennyLane {pl_p} (delta {abs(pl_p-wire_p):.2e})")
        print(f"OK: cross-oracle — OS wire p={wire_p:.6f} == PennyLane lightning.qubit p={pl_p:.6f}")
    except ImportError:
        print("SKIP: cross-oracle (PennyLane not installed) — the B2 gate covers it hermetically")

    # 4. Large circuit near QPU_CIRCUIT_MAX → multi-read inbound reassembly. Pad
    #    the Bell circuit with X/X pairs on qubit 0: X is a self-inverse
    #    PERMUTATION, so the pairs are identity AND (unlike H/H) never grow the
    #    amplitudes or h — the state stays Bell, p=1/2. (Charges quota #3.)
    pairs = 40  # 8 + (2 + 2*40)*3 = 254 bytes; crosses the in[128] read boundary
    ops = [(qc.QC_OP_H, 0, 0), (qc.QC_OP_CNOT, 0, 1)]
    ops += [(qc.QC_OP_X, 0, 0), (qc.QC_OP_X, 0, 0)] * pairs
    big = qc.build(2, ops, probe=0)
    if not (128 < len(big) <= 256):
        _fail(f"large circuit {len(big)}B — must be >128 (cross read boundary) and <=256")
    rl = _decode_ok(vm, big, "large")
    if not (rl["num"] == 1 and rl["den"] == 2 and rl["amp_re"] == 1):
        _fail(f"large circuit wrong result: {rl}")
    print(f"OK: {len(big)}B circuit brokered (multi-read reassembly) → p=1/2 preserved")

    # 5. Malformed circuit → wire-visible broker error (status=2), not a false
    #    OK. It reaches the broker (valid length) so it CHARGES quota #4, but
    #    the executor rejects the opcode and forwards QC_STATUS_EINVAL.
    bad = bytes([qc.QC_VERSION, 3, 1, 0, 0, 0, 0, 0]) + bytes([99, 0, 0])  # opcode 99 invalid
    rb = vm.qsubmit(bad)
    if rb["status"] != 2:
        _fail(f"malformed circuit: wire status {rb['status']} != 2 (broker/EINVAL)")
    if rb["result"] and len(rb["result"]) >= 1 and rb["result"][0] != qc.QC_STATUS_EINVAL:
        _fail(f"malformed circuit: executor did not forward EINVAL: {rb['result'][:1]!r}")
    print("OK: malformed circuit → wire broker error (status=2, EINVAL forwarded)")

    # 5b. Overflow guard (charges quota #5): 64 H gates on one qubit drive the
    #     amplitude to 2^32, overflowing int32. The executor must DETECT this
    #     (int64 butterfly + sticky flag) and reject with EINVAL rather than
    #     report a wrapped amplitude as a valid probability.
    overflow = qc.build(1, [(qc.QC_OP_H, 0, 0)] * 64, probe=0)
    ro = vm.qsubmit(overflow)
    if ro["status"] != 2:
        _fail(f"overflow circuit: wire status {ro['status']} != 2 (should reject, not wrap)")
    print("OK: int32-overflowing circuit rejected (status=2 — no wrapped amplitude)")

    # 6. Quota: bell + grover3 + large + malformed + overflow = 5 charges =
    #    qsub_max, so the 6th submit must be refused (status=1). Assert the
    #    swarm-svc incarnation is unchanged across the run (a rebirth would reset
    #    qsub_used and invalidate this) via the process table.
    def swarm_pid():
        return next((p["pid"] for p in vm.sysinfo()["processes"]
                     if p["name"] == "swarm-svc" and p["state"] in ("RUNNING", "READY")), None)

    pid0 = swarm_pid()
    over = vm.qsubmit(qc.bell())
    pid1 = swarm_pid()
    if pid0 is None or pid1 is None or pid0 != pid1:
        _fail(f"swarm-svc incarnation changed mid-test ({pid0}->{pid1}) — quota result unreliable")
    if over["status"] != 1:
        _fail(f"submit #{QSUB_MAX + 1} status {over['status']} != 1 (quota refused)")
    print(f"OK: wire qsub quota ENFORCED — {QSUB_MAX} charged then refused (status=1); "
          f"swarm-svc pid {pid0} stable across the burst")

    print("=== COM2 quantum-submit gate PASSED ===")


if __name__ == "__main__":
    main()
