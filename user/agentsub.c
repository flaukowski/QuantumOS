/**
 * QuantumOS agentsub — the sub-agent in the agentd end-to-end demo.
 *
 * Capless by design (no grant flags, empty manifest): it holds NO field
 * capability of its own. At boot the agent (agentd) hands it, via SYS_CAP_DERIVE
 * over a capability-checked IPC pair, a strictly-NARROWED READ-only slice of
 * field region 3. That derive also EXTENDS agentsub's manifest with a FIELD:3
 * allow row it could never obtain on its own — so its very existence proves a
 * RUNTIME cross-ring delegation happened.
 *
 * Protocol: announce "ready" (a first-match send reaches agentd, which learns
 * this pid from the vouched sender field); await agentd's "go" (sent only after
 * the derive); recall region 3 with the delegated READ cap (must succeed); prove
 * WRITE was narrowed away (imprint must be EPERM); then ack "proven". Not
 * monitored (a restart would rebind the manifest and drop the delegated row).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define AGENT_REGION 3
/* Must match agentd.c — the phrase agentd imprints into region 3. */
static const unsigned char AGENT_PHRASE[] = "agentd end to end field phrase";
#define AGENT_PHRASE_LEN (sizeof(AGENT_PHRASE) - 1)

static long sub_recall(void) {
    field_recall_req_t rreq;
    field_recall_out_t rout;
    for (unsigned i = 0; i < sizeof(rreq); i++) {
        ((unsigned char *)&rreq)[i] = 0;
    }
    rreq.region = AGENT_REGION;
    rreq.len = AGENT_PHRASE_LEN;
    rreq.k = 1;
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        rreq.probe[n] = AGENT_PHRASE[n];
    }
    return recall_(&rreq, &rout);
}

void _start(void) {
    unsigned self = (unsigned)getpid();

    send_msg("ready", 5);

    char buf[16];
    int got_go = 0;
    for (long spins = 0; spins < 100000000L && !got_go; spins++) {
        if (recv_msg(buf, sizeof(buf)) != 0 && buf[0] == 'g') {
            got_go = 1;
        } else {
            yield();
        }
    }
    char line[80];
    if (!got_go) {
        write_str("AGENTSUB BROKEN no go");
        for (;;) {
            yield();
        }
    }

    /* Delegated READ must recall; withheld WRITE must be EPERM (-4). */
    long r = sub_recall();

    field_imprint_req_t ireq;
    for (unsigned i = 0; i < sizeof(ireq); i++) {
        ((unsigned char *)&ireq)[i] = 0;
    }
    ireq.region = AGENT_REGION;
    ireq.len = 3;
    ireq.pattern[0] = 'n';
    ireq.pattern[1] = 'o';
    ireq.pattern[2] = 'w';
    long w = imprint_(&ireq);

    if (r == 0 && w == -4) {
        snprintf(line, sizeof(line), "AGENTSUB: delegated recall OK, write narrowed (pid=%u)",
                 self);
        write_str(line);
        send_msg("proven", 6);
    } else {
        snprintf(line, sizeof(line), "AGENTSUB BROKEN recall=%d imprint=%d", (int)r, (int)w);
        write_str(line);
    }

    /* One-shot proof: exit so this citizen leaves the ready queue (see agentd). */
    exit_(0);
    for (;;) {
        yield();
    }
}
