/**
 * QuantumOS subagentd — the RECIPIENT of a delegated capability (epic #137
 * Phase D increment 3).
 *
 * A ring-3 service that holds NO field capability of its own (its declared
 * manifest is empty — it has no grant flags). At boot the delegator
 * (delegation_test) hands it, via SYS_CAP_DERIVE, a strictly-NARROWED READ-only
 * capability over field region 2 (the delegator holds READ|WRITE|GRANT there).
 * That derive also EXTENDS subagentd's manifest with a FIELD:2 allow row — a
 * row it structurally could never obtain at boot (no field grant), so the row's
 * mere existence is the un-echoable, un-fakeable proof that a RUNTIME cross-ring
 * delegation happened.
 *
 * It proves the NARROWING two ways:
 *   - recall(region 2) MUST succeed (it holds the delegated READ cap; a capless
 *     recall would be EPERM before touching any field memory), and
 *   - imprint(region 2) MUST be EPERM (-4) — WRITE was narrowed away.
 * On {recall ok, imprint EPERM} it prints "DELEG ENFORCED pid=<self>" and acks
 * the delegator. It then proves DELEGATED provenance (vs a static grant): once
 * the delegator exits, the idle reaper cascade-revokes the derived cap, so
 * subagentd's next recall flips to EPERM → it prints "DELEG REVOKED pid=<self>".
 * A static grant would survive an unrelated process's death; only a cap DERIVED
 * from the delegator's cascades. Any other outcome → "DELEG BROKEN ...".
 *
 * Registered but NOT monitored (like quota_test): it must not be watchdog-
 * restarted (that would rebind its manifest and drop the delegated row).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

/* The phrase the delegator imprints into region 2; recalled here to exercise
 * the delegated READ cap. VARIED bytes (a degenerate all-equal pattern fails to
 * imprint, field.h). Byte array + sizeof-1 so the copy is bounded to its own
 * length (< FIELD_PAT_MAX). Must match delegation_test.c. */
static const unsigned char DELEG_PHRASE[] = "delegated region two secret phrase";
#define DELEG_PHRASE_LEN (sizeof(DELEG_PHRASE) - 1)
#define DELEG_REGION 2

static long deleg_recall(void) {
    field_recall_req_t rreq;
    field_recall_out_t rout;
    for (unsigned i = 0; i < sizeof(rreq); i++) {
        ((unsigned char *)&rreq)[i] = 0;
    }
    rreq.region = DELEG_REGION;
    for (unsigned n = 0; n < DELEG_PHRASE_LEN; n++) {
        rreq.probe[n] = DELEG_PHRASE[n];
    }
    rreq.len = DELEG_PHRASE_LEN;
    rreq.k = 1;
    return recall_(&rreq, &rout);
}

void _start(void) {
    unsigned self = (unsigned)getpid();

    /* Announce readiness — our single IPC cap points to the delegator, so a
     * first-match send reaches it; it learns our pid from the sender field. */
    send_msg("ready", 5);

    /* Wait (bounded) for the delegator's "go" — it derives our cap first. */
    char buf[16];
    int got_go = 0;
    for (long spins = 0; spins < 8000000L && !got_go; spins++) {
        if (recv_msg(buf, sizeof(buf)) != 0 && buf[0] == 'g') {
            got_go = 1;
        } else {
            yield();
        }
    }
    char line[80];
    if (!got_go) {
        write_str("DELEG BROKEN recall=-98 imprint=-98");
        for (;;) {
            yield();
        }
    }

    /* Prove the narrowing: recall (READ granted) then imprint (WRITE withheld). */
    long r = deleg_recall();

    field_imprint_req_t ireq;
    for (unsigned i = 0; i < sizeof(ireq); i++) {
        ((unsigned char *)&ireq)[i] = 0;
    }
    ireq.region = DELEG_REGION;
    ireq.len = 5;
    ireq.pattern[0] = 'w';
    ireq.pattern[1] = 'r';
    ireq.pattern[2] = 'i';
    ireq.pattern[3] = 't';
    ireq.pattern[4] = 'e';
    long w = imprint_(&ireq);

    if (r == 0 && w == -4) {
        snprintf(line, sizeof(line), "DELEG ENFORCED pid=%u", self);
        write_str(line);
        send_msg("proven", 6); /* let the delegator exit -> cascade revoke */

        /* Cascade-revoke proof: once the delegator dies, our derived cap is
         * revoked and recall flips to EPERM. This is the ONLY signal a static
         * grant cannot fake. */
        int revoked = 0;
        for (long spins = 0; spins < 40000000L && !revoked; spins++) {
            if (deleg_recall() == -4) {
                snprintf(line, sizeof(line), "DELEG REVOKED pid=%u", self);
                write_str(line);
                revoked = 1;
            } else {
                yield();
            }
        }
        if (!revoked) {
            write_str("DELEG BROKEN recall=0 imprint=-4 (revoke not observed)");
        }
    } else {
        snprintf(line, sizeof(line), "DELEG BROKEN recall=%d imprint=%d", (int)r, (int)w);
        write_str(line);
    }

    for (;;) {
        yield();
    }
}
