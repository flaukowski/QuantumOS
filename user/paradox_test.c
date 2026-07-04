/**
 * QuantumOS paradox-test — boot self-test client for paradoxd (ghostOS
 * phase 3, #50).
 *
 * It hands paradoxd a single deterministic contradiction (the canned
 * x = 1/x seed plus a spread vector, both fixed in paradox.h — no randomness
 * on the gate path) over capability-checked IPC. paradoxd resolves it and
 * prints the merge-gate line itself:
 *
 *     PARADOXD: RESOLVED n=<iters> phase=C
 *
 * ci-smoke and the CI boot test grep for "PARADOXD: RESOLVED" the same way
 * they gate on "QuantumOS ready" and "GHOSTD: 3/3 RECALL OK".
 *
 * Proof-by-attack (capability discipline): paradox-test holds exactly one
 * IPC send-cap — to paradoxd — so its send of the contradiction is
 * authorised, but a targeted send to any *other* address is denied with
 * EPERM before delivery. Capability-as-address is per-destination: the same
 * gate is what keeps a process lacking paradoxd's cap from reaching paradoxd.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "paradox.h"

void _start(void) {
    write_str("paradox-test: online — handing paradoxd a canned contradiction");

    /* Proof-by-attack: a targeted send to an address we hold no capability
     * for must be denied (EPERM = -4). We only hold a send-cap to paradoxd;
     * addressing anything else (here pid 1) is refused by the capability
     * check — the same rule that denies a capless caller a path to paradoxd. */
    {
        const char *probe = "unauthorised";
        long rc = send_to(1, probe, str_len(probe));
        if (rc == -4) {
            write_str("PARADOXD: capless send denied (EPERM)");
        } else {
            write_str("PARADOXD: WARNING — unauthorised send was NOT denied");
        }
    }

    /* The canned contradiction (deterministic). Routed to paradoxd by our
     * single IPC send-cap (capability-as-address). */
    paradox_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++)
        ((uint8_t *)&req)[i] = 0;
    req.op = PARADOX_RESOLVE;
    req.x0 = PARADOX_GATE_X0;
    req.vec[0] = PARADOX_GATE_V0;
    req.vec[1] = PARADOX_GATE_V1;
    req.vec[2] = PARADOX_GATE_V2;
    req.vec[3] = PARADOX_GATE_V3;

    if (send_msg((const char *)&req, (long)sizeof(req)) != 0) {
        write_str("paradox-test: handoff FAILED (send denied)");
        exit_(1);
    }

    write_str("paradox-test: contradiction delivered, exiting");
    exit_(0);
    for (;;) {
    }
}
