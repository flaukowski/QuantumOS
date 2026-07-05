/**
 * QuantumOS ghost_test — boot self-test client for ghostd (#48).
 *
 * Imprints three distinct patterns, then recalls each from a
 * deterministically corrupted probe (~15% of bits flipped — every 7th
 * bit, no randomness on the gate path). It talks to ghostd over
 * capability-checked IPC (it holds exactly one IPC send-cap, to ghostd)
 * and prints a single merge-gate line:
 *
 *     GHOSTD: 3/3 RECALL OK R=0.xx
 *
 * ci-smoke and the CI boot test grep for "GHOSTD: 3/3 RECALL OK" the
 * same way they gate on "QuantumOS ready".
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h"

/* Deterministic ~15% corruption: flip every 7th bit (37/256 = 14.5%). */
static void corrupt(uint32_t *pat) {
    for (int i = 0; i < GHOST_N; i += 7)
        ghost_setbit(pat, i, !ghost_bit(pat, i));
}

/* Send one request and block (bounded) for ghostd's reply. */
static int request(const ghost_req_t *req, ghost_rep_t *rep) {
    if (send_msg((const char *)req, (long)sizeof(*req)) != 0) {
        return 0;
    }
    char buf[sizeof(ghost_rep_t) + 8];
    for (long spins = 0; spins < 4000000; spins++) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender != 0) {
            for (unsigned i = 0; i < sizeof(*rep); i++)
                ((uint8_t *)rep)[i] = (uint8_t)buf[i];
            return 1;
        }
        yield();
    }
    return 0;
}

void _start(void) {
    write_str("ghost-test: online — imprinting 3 patterns, then noisy recall");

    /* Proof-by-attack (same tradition as the rogue process): ghost-test
     * holds only an IPC send-cap to ghostd and NO quantum-pool capability,
     * so its SYS_QRAND draw must be denied with EPERM (-4) before any byte
     * is produced. The capability gate — not a convention — is what stops it. */
    {
        uint8_t qbuf[8];
        long qr = qrand_fill(qbuf, (long)sizeof(qbuf));
        if (qr == -4) {
            write_str("QRAND: capless caller denied (EPERM)");
        } else {
            write_str("QRAND: WARNING — capless caller was NOT denied");
        }
    }

    /* Same proof-by-attack for the COM2 swarm-bridge device (ghostd phase 4):
     * ghost-test holds no CAP_RESOURCE_DEVICE cap over COM2, so its write must
     * be denied EPERM (-4). Only swarm_svc is granted the device capability. */
    {
        uint8_t one = 0x2A;
        long cr = com2_write_bytes(&one, 1);
        if (cr == -4) {
            write_str("COM2: capless caller denied (EPERM)");
        } else {
            write_str("COM2: WARNING — capless caller was NOT denied");
        }
    }

    /* And for the interactive console (epic #62 phase 1): ghost-test holds
     * no CAP_RESOURCE_DEVICE cap over DEVICE_ID_CONSOLE, so it can neither
     * read keystrokes nor forge console output — only qsh is granted the
     * console. The read attempt must be denied EPERM before the input ring
     * is even looked at. */
    {
        uint8_t kbuf[4];
        long kr = cons_read(kbuf, (long)sizeof(kbuf));
        if (kr == -4) {
            write_str("CONS: capless caller denied (EPERM)");
        } else {
            write_str("CONS: WARNING — capless caller was NOT denied");
        }
    }

    /* And for program execution (epic #62 phase 3): spawning is real
     * authority — a process that can start programs can multiply. Only
     * qsh holds the CAP_RESOURCE_PROCESS spawn capability, so this
     * attempt must be denied EPERM before the initrd is even consulted. */
    {
        long sr = spawn_("/bin/hello");
        if (sr == -4) {
            write_str("SPAWN: capless caller denied (EPERM)");
        } else {
            write_str("SPAWN: WARNING — capless caller was NOT denied");
        }
    }

    /* And for filesystem writes (epic #71): only qsh holds the
     * filesystem-write capability, so a capless create must be denied
     * EPERM before the overlay is even touched. */
    {
        long fr = openf_("/data/forged", O_WRONLY | O_CREAT | O_TRUNC);
        if (fr == -4) {
            write_str("FSW: capless caller denied (EPERM)");
        } else {
            write_str("FSW: WARNING — capless caller was NOT denied");
        }
    }

    /* And for network access (epic #73 follow-up): only qsh holds the
     * network capability, so a capless resolve must be denied EPERM
     * before any lookup is posted. */
    {
        unsigned char ip[4];
        long nr = resolve_("example.com", ip);
        if (nr == -4) {
            write_str("NETC: capless caller denied (EPERM)");
        } else {
            write_str("NETC: WARNING — capless caller was NOT denied");
        }
    }

    const uint32_t seeds[3] = {GHOST_SEED_0, GHOST_SEED_1, GHOST_SEED_2};
    uint32_t pats[3][GHOST_PW];

    /* Imprint */
    for (int p = 0; p < 3; p++) {
        ghost_gen_pattern(pats[p], seeds[p]);
        ghost_req_t req;
        for (unsigned i = 0; i < sizeof(req); i++)
            ((uint8_t *)&req)[i] = 0;
        req.op = GHOST_REMEMBER;
        req.slot = (uint8_t)p;
        for (int w = 0; w < GHOST_PW; w++)
            req.bits[w] = pats[p][w];
        ghost_rep_t rep;
        if (!request(&req, &rep)) {
            write_str("ghost-test: imprint FAILED (no reply)");
            exit_(1);
        }
    }

    /* Recall from corrupted probes */
    int ok = 0;
    uint64_t r_sum = 0;
    for (int p = 0; p < 3; p++) {
        uint32_t probe[GHOST_PW];
        for (int w = 0; w < GHOST_PW; w++)
            probe[w] = pats[p][w];
        corrupt(probe);

        ghost_req_t req;
        for (unsigned i = 0; i < sizeof(req); i++)
            ((uint8_t *)&req)[i] = 0;
        req.op = GHOST_RECALL;
        for (int w = 0; w < GHOST_PW; w++)
            req.bits[w] = probe[w];

        ghost_rep_t rep;
        if (request(&req, &rep) && rep.match == (int8_t)p) {
            ok++;
            r_sum += rep.r_q16;
        }
    }

    /* The one gate line. R is the mean order parameter of the recalls. */
    uint32_t r_avg = ok ? (uint32_t)(r_sum / (uint64_t)ok) : 0;
    char line[64];
    int o = ghost_put(line, 0, "GHOSTD: ");
    o = ghost_put_u(line, o, (unsigned)ok);
    o = ghost_put(line, o, "/3 RECALL OK R=");
    o = ghost_put_r(line, o, r_avg);
    line[o] = 0;
    write_str(line);

    exit_(0);
    for (;;) {
    }
}
