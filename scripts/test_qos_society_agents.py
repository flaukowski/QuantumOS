"""QuantumOS society-of-societies gate (epic #178). Stdlib-only, like the other
society gates: drives QosSociety directly and never imports `mcp`, so it runs in
the integration CI job with no pip.

Boots TWO attested QuantumOS VMs with BOTH the `agentdemo` token AND the
coupling wire, and proves, anti-vacuously:
  1. each VM runs its OWN complete agent society locally: spawn-assembled
     specialists, content consensus, division of labor, and the qseed-salted
     AGGREGATE handoff (the full AGENTD: DEMO OK conjunction, per VM);
  2. the two societies EXCHANGE results: each VM's COM1 shows
     "FIELDSYNC: peer aggregate a<hex>" carrying the OTHER VM's aggregate —
     the host recomputes both expected values from the compile-time phrase +
     workspace digits + each boot's qseed, so a forged/looped-back local value
     cannot satisfy the assertion (the two aggregates provably DIFFER because
     the qseeds differ, which boot() already enforces);
  3. the underlying fields also synchronize (the existing society property) —
     with a GENEROUS budget, because the agentdemo work (a QPU job, four
     spawns, field ops) rides the same boots (the #171 timing lesson);
  4. shutdown reaps BOTH QEMU processes.

This gate is NEW rather than an extension of test_qos_society.py so the
existing society gates keep their demo-free timing (design-review finding).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosSociety  # noqa: E402

QSEED_A = "1111111111111111"
QSEED_B = "8888888888888888"

# Mirrors user/agentd.c exactly: the aggregate is FNV-1a over the three
# specialist result strings ("w%08x" of the phrase||digit digest), salted with
# hi32^lo32 of the boot qseed.
PHRASE = b"agentd end to end field phrase"
FNV_PRIME = 16777619
FNV_BASIS = 2166136261
M32 = 0xFFFFFFFF


def _fnv1a(data, h=FNV_BASIS):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M32
    return h


def expected_aggregate(qseed_hex):
    agg = FNV_BASIS
    for ws in (4, 5, 6):
        rd = _fnv1a(PHRASE)
        rd = ((rd ^ (ord("0") + ws)) * FNV_PRIME) & M32
        for b in ("w%08x" % rd).encode():
            agg = ((agg ^ b) * FNV_PRIME) & M32
    # Identity salt mirrors agentd: FNV-1a over the qseed's 8 LE bytes (a
    # plain hi32^lo32 fold is ZERO for palindromic qseeds like the CI pair).
    q = int(qseed_hex, 16)
    return agg ^ _fnv1a(q.to_bytes(8, "little"))


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _await(pred, deadline, what):
    while time.time() < deadline:
        if pred():
            return
        time.sleep(0.5)
    _fail(f"timeout awaiting {what}")


def main():
    kernel = os.environ.get("QOS_KERNEL") or None
    soc = QosSociety(kernel=kernel)
    exp_a = expected_aggregate(QSEED_A)
    exp_b = expected_aggregate(QSEED_B)
    if exp_a == exp_b:
        _fail("expected aggregates identical — qseed salt dead, gate vacuous")
    own_re = re.compile(r"AGENT: aggregate a([0-9a-f]{8}) handed to fieldsyncd")
    peer_re = re.compile(r"FIELDSYNC: peer aggregate a([0-9a-f]{8})")
    try:
        soc.boot(QSEED_A, QSEED_B, timeout=60, extra_tokens="agentdemo")
        print("OK: two attested members booted with agentdemo + coupling")

        # 1. Each VM completes its OWN society end to end (DEMO OK is a
        # conjunction over qpu+field+spawn+society+division+aggregate).
        deadline = time.time() + 90.0
        _await(lambda: "AGENTD: DEMO OK" in soc.a._log_text()
               and "AGENTD: DEMO OK" in soc.b._log_text(),
               deadline, "AGENTD: DEMO OK on both members")
        for vm, name, exp in ((soc.a, "A", exp_a), (soc.b, "B", exp_b)):
            text = vm._log_text()
            if "AGENT: division of labor 3/3" not in text:
                _fail(f"member {name}: no division-of-labor proof")
            m = own_re.search(text)
            if not m:
                _fail(f"member {name}: no aggregate handoff line")
            if int(m.group(1), 16) != exp:
                _fail(f"member {name}: own aggregate {m.group(1)} != expected "
                      f"{exp:08x} — recompute drifted from agentd")
        print(f"OK: both societies complete; aggregates a{exp_a:08x} / a{exp_b:08x} verified")

        # 2. EXCHANGE: each member's console must show the OTHER's aggregate,
        # delivered as an FSYP frame from a validated peer source.
        deadline = time.time() + 60.0
        _await(lambda: any(int(v, 16) == exp_b
                           for v in peer_re.findall(soc.a._log_text())),
               deadline, "member A receiving B's aggregate")
        _await(lambda: any(int(v, 16) == exp_a
                           for v in peer_re.findall(soc.b._log_text())),
               deadline, "member B receiving A's aggregate")
        # Distinctness (anti-vacuous): a looped-back/forged LOCAL value can
        # never satisfy the cross assertion above, but assert it explicitly.
        for vm, name, own in ((soc.a, "A", exp_a), (soc.b, "B", exp_b)):
            for v in peer_re.findall(vm._log_text()):
                if int(v, 16) == own:
                    _fail(f"member {name} printed its OWN aggregate as a peer's "
                          "— self-frame leaked through peer validation")
        print("OK: societies exchanged results — A holds B's aggregate and vice versa")

        # 3. The fields still synchronize under the demo load (generous budget).
        deadline = time.time() + 120.0
        _await(lambda: (lambda s: s["a"]["synchronized"] and s["b"]["synchronized"])
               (soc.status()), deadline, "cross-VM field sync (R_x)")
        print("OK: both fields synchronized under agentdemo load")
    finally:
        soc.shutdown()
    if soc.a is not None or soc.b is not None:
        _fail("shutdown did not clear the society's handles")
    print("=== SOCIETY-OF-SOCIETIES gate PASSED — two societies, one field, "
          "results exchanged ===")


if __name__ == "__main__":
    main()
