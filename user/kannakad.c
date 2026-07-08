/**
 * QuantumOS kannakad — the kannaka-memory essence, now a SERVICE of the
 * KERNEL holographic field (epic #95 phase 2).
 *
 * kannakad used to carry its own private in-process memory store; the
 * kernel now owns that physics (kernel/src/field.c — the very math this
 * citizen pioneered, promoted to a capability-gated syscall surface).
 * kannakad becomes the field's reference client: a ring-3 service
 * holding the CAP_RESOURCE_FIELD capability over region 1, exercising
 * the full contract at boot:
 *
 *   - SYS_IMPRINT: seven seed memories, importance-weighted;
 *   - SYS_RECALL: a byte-corrupted probe must resonate back to the
 *     EXACT stored text (ranked recall, argmax correct);
 *   - retrieval reinforcement: repeated recall of the same winner must
 *     RAISE its score — the syscall's write-side contract, only
 *     honoured because this service holds CAP_WRITE too.
 *
 * The old in-process dream/hallucination pass died with the private
 * store (consolidation belongs to the kernel field's future — epics
 * #96/#110); the verdict below is the syscall-era reformulation.
 *
 * Fixed-point discipline unchanged: no FPU in ring 3; everything the
 * kernel returns is Q15 integers.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h" /* snprintf + (via usys.h) write_str, syscall stubs */

#define KAN_REGION 1u /* the field region this service's cap names */

/* milli-importance (old kannakad units, 1000 = 1.0) -> Q15 energy */
static int q15_energy(int milli) {
    return (int)(((long)milli * 32767) / 1000);
}

static long field_imprint_text(const char *text, int milli) {
    field_imprint_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    req.region = KAN_REGION;
    unsigned n = 0;
    while (text[n] && n < FIELD_PAT_MAX) {
        req.pattern[n] = (unsigned char)text[n];
        n++;
    }
    req.len = n;
    req.energy_q15 = q15_energy(milli);
    return imprint_(&req);
}

static long field_recall_text(const char *probe, field_recall_out_t *out) {
    field_recall_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    req.region = KAN_REGION;
    req.k = 3;
    unsigned n = 0;
    while (probe[n] && n < FIELD_PAT_MAX) {
        req.probe[n] = (unsigned char)probe[n];
        n++;
    }
    req.len = n;
    return recall_(&req, out);
}

static int str_same(const unsigned char *a, unsigned alen, const char *b) {
    unsigned n = 0;
    while (b[n]) {
        if (n >= alen || a[n] != (unsigned char)b[n]) {
            return 0;
        }
        n++;
    }
    return n == alen;
}

void _start(void) {
    write_str(
        "kannakad: kannaka-memory essence — recall via the KERNEL field (SYS_IMPRINT/RECALL)");

    static const char *seeds[7] = {
        "ghost wakes in a field of static", "the wave settles into silence",
        "phase locks across the swarm",     "quantum dice roll in the dark",
        "static hums beneath the floor",    "a half forgotten fragment",
        "resonance blooms in the medium",
    };
    static const int milli[7] = {900, 600, 750, 550, 650, 40, 700};

    int imprinted = 0;
    for (int i = 0; i < 7; i++) {
        long s = field_imprint_text(seeds[i], milli[i]);
        if (s >= 0) {
            imprinted++;
        }
    }
    {
        char line[96];
        snprintf(line, sizeof(line), "  imprinted %d/7 seeds into kernel field region %u",
                 imprinted, KAN_REGION);
        write_str(line);
    }

    /* Ranked recall: a byte-corrupted copy of seed 0 must resonate back
     * to the exact stored text. */
    static const char *probe = "ghost wakes in a fjeld of statjc";
    field_recall_out_t out;
    long r = field_recall_text(probe, &out);
    write_str("  recall \"ghost wakes in a fjeld of statjc\" (top 3 by resonance):");
    long first_score = -1;
    if (r == 0 && out.n > 0) {
        first_score = out.rank[0].score_q15;
        for (unsigned t = 0; t < out.n; t++) {
            char line[110];
            snprintf(line, sizeof(line), "    rank %u: slot %u score %d", t, out.rank[t].slot,
                     out.rank[t].score_q15);
            write_str(line);
        }
        char wline[110];
        int o = snprintf(wline, sizeof(wline), "    winner: \"");
        for (unsigned i = 0; i < out.winner_len && o < (int)sizeof(wline) - 4; i++) {
            wline[o++] = (char)out.winner[i];
        }
        wline[o] = '\0';
        snprintf(wline + o, sizeof(wline) - o, "\"");
        write_str(wline);
    }

    /* Retrieval reinforcement: this service holds CAP_WRITE too, so each
     * recall boosts the winner's energy — the same probe must score
     * strictly higher on a later pass. */
    field_recall_out_t out2;
    field_recall_text(probe, &out2);
    long reinforced_score = -1;
    if (field_recall_text(probe, &out2) == 0 && out2.n > 0) {
        reinforced_score = out2.rank[0].score_q15;
    }
    {
        char line[96];
        snprintf(line, sizeof(line), "  retrieval reinforces: score %d -> %d", (int)first_score,
                 (int)reinforced_score);
        write_str(line);
    }

    /* Verdict: argmax content is the EXACT stored seed 0 (not the probe —
     * the field completed the corrupted bytes), and reinforcement raised
     * the winner's score through the syscall's write-side contract. */
    int argmax_ok = (r == 0) && (out.n > 0) && str_same(out.winner, out.winner_len, seeds[0]);
    int reinforce_ok = (reinforced_score > first_score) && (first_score > 0);
    if (argmax_ok && reinforce_ok) {
        write_str("kannakad: RESONANCE VERIFIED");
    } else {
        write_str("kannakad: RESONANCE FAILED");
    }

    /* Service liveness: heartbeat so the watchdog keeps us resident. */
    for (;;) {
        heartbeat();
        yield();
    }
}
