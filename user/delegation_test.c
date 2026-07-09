/**
 * QuantumOS delegation_test — the DELEGATOR (epic #137 Phase D increment 3).
 *
 * The one citizen granted CAP_GRANT (grant_field_delegable over field region 2:
 * CAP_READ|CAP_WRITE|CAP_GRANT). At boot it hands a strictly-NARROWED READ-only
 * slice of that region to its sub-agent (subagentd) via SYS_CAP_DERIVE — the
 * realization of "an agent hands a narrowed intent to a sub-agent." The kernel
 * bounds the derive by this delegator's OWN intent manifest (it may only
 * delegate a resource it is declared to touch) and extends the recipient's
 * manifest so the delegated cap is usable.
 *
 * Flow: learn the sub-agent's pid from its authentic IPC "ready" (recv returns
 * the real sender pid); imprint a seed phrase into region 2 (it holds WRITE);
 * SYS_CAP_DERIVE a READ-only cap to the sub-agent; print "DELEG ISSUED sub=<S>"
 * (a DISTINCT token — the NARROWING verdict is the sub-agent's ENFORCED, never
 * the delegator's); signal "go"; await the "proven" ack; then EXIT. Being
 * unmonitored, it is not restarted — the idle reaper cascade-revokes the
 * derived cap, which is how the sub-agent proves DELEGATED (not static)
 * provenance.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

/* The phrase seeded into region 2; VARIED bytes (a degenerate all-equal
 * pattern fails to imprint). Must match subagentd.c. Byte array + sizeof-1 so
 * the copy is bounded to its own length (< FIELD_PAT_MAX). */
static const unsigned char DELEG_PHRASE[] = "delegated region two secret phrase";
#define DELEG_PHRASE_LEN (sizeof(DELEG_PHRASE) - 1)
#define DELEG_REGION 2

void _start(void) {
    /* Learn the sub-agent's pid from its authentic "ready" (kernel-vouched
     * sender). Bounded wait — the sub-agent sends on start. */
    char buf[16];
    long sub = 0;
    for (long spins = 0; spins < 8000000L && !sub; spins++) {
        long s = recv_msg(buf, sizeof(buf));
        if (s != 0) {
            sub = s;
        } else {
            yield();
        }
    }
    char line[80];
    if (!sub) {
        write_str("DELEG BROKEN recall=-97 imprint=-97");
        for (;;) {
            yield();
        }
    }

    /* Seed region 2 (we hold WRITE) so the sub-agent's recall has content. */
    field_imprint_req_t seed;
    for (unsigned i = 0; i < sizeof(seed); i++) {
        ((unsigned char *)&seed)[i] = 0;
    }
    seed.region = DELEG_REGION;
    for (unsigned n = 0; n < DELEG_PHRASE_LEN; n++) {
        seed.pattern[n] = DELEG_PHRASE[n];
    }
    seed.len = DELEG_PHRASE_LEN;
    imprint_(&seed);

    /* Delegate a READ-only slice of region 2 to the sub-agent. */
    cap_derive_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    req.resource_type = CAP_RESOURCE_FIELD;
    req.resource_id = DELEG_REGION;
    req.permissions = CAP_READ; /* narrowed: no WRITE, no GRANT */
    req.target_pid = (unsigned)sub;
    req.expiration = 0;
    long d = cap_derive_(&req);

    snprintf(line, sizeof(line), "DELEG ISSUED sub=%d rc=%d", (int)sub, (int)d);
    write_str(line);

    /* Tell the sub-agent the cap is ready. */
    send_msg("go", 2);

    /* Await the sub-agent's "proven" ack, then exit so the reaper cascade-
     * revokes the derived cap (the sub-agent then observes DELEG REVOKED). */
    int proven = 0;
    for (long spins = 0; spins < 40000000L && !proven; spins++) {
        if (recv_msg(buf, sizeof(buf)) != 0 && buf[0] == 'p') {
            proven = 1;
        } else {
            yield();
        }
    }

    exit_(0); /* unmonitored -> not restarted -> derived cap cascade-revoked */
    for (;;) {
    }
}
