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
 * the derive); recall region 3 with the delegated READ cap from a probe
 * corrupted at PID-SALTED positions (every society member reconstructs the
 * phrase from a DIFFERENT noisy probe) and verify the exact bytes came back;
 * prove WRITE was narrowed away (imprint must be EPERM); then ack
 * "p<8-hex-FNV1a-of-the-recalled-winner>" so the orchestrator can check the
 * CONTENT each sub actually recalled, not merely that a recall succeeded. Not
 * monitored (a restart would rebind the manifest and drop the delegated row).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define AGENT_REGION 3
/* Must match agentd.c — the phrase agentd imprints into region 3. */
static const unsigned char AGENT_PHRASE[] = "agentd end to end field phrase";
#define AGENT_PHRASE_LEN (sizeof(AGENT_PHRASE) - 1)

/* FNV-1a over the recalled winner — MUST mirror fnv1a() in agentd.c, which
 * digests the phrase independently and compares every sub's ack against it. */
static unsigned int fnv1a(const unsigned char *p, unsigned len) {
    unsigned int h = 2166136261u;
    for (unsigned i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Recall region 3 from a probe corrupted at two PID-SALTED positions (XOR 0x01
 * keeps every byte printable) — each society member reconstructs from its OWN
 * noise. The recalled winner lands in *rout for content verification. */
static long sub_recall(unsigned self, field_recall_out_t *rout) {
    field_recall_req_t rreq;
    for (unsigned i = 0; i < sizeof(rreq); i++) {
        ((unsigned char *)&rreq)[i] = 0;
    }
    rreq.region = AGENT_REGION;
    rreq.len = AGENT_PHRASE_LEN;
    rreq.k = 1;
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        rreq.probe[n] = AGENT_PHRASE[n];
    }
    rreq.probe[self % AGENT_PHRASE_LEN] ^= 0x01;
    rreq.probe[(self * 7u + 3u) % AGENT_PHRASE_LEN] ^= 0x01;
    return recall_(&rreq, rout);
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
    char line[112];
    if (!got_go) {
        write_str("AGENTSUB BROKEN no go");
        for (;;) {
            yield();
        }
    }

    /* Delegated READ must recall the EXACT phrase from this sub's own noisy
     * probe; withheld WRITE must be EPERM (-4). */
    field_recall_out_t rout;
    for (unsigned i = 0; i < sizeof(rout); i++) {
        ((unsigned char *)&rout)[i] = 0;
    }
    long r = sub_recall(self, &rout);
    int content_ok = (r == 0 && rout.winner_len == AGENT_PHRASE_LEN);
    for (unsigned n = 0; content_ok && n < AGENT_PHRASE_LEN; n++) {
        if (rout.winner[n] != AGENT_PHRASE[n]) {
            content_ok = 0;
        }
    }

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

    if (content_ok && w == -4) {
        /* Ack carries evidence: the digest of what THIS sub actually recalled,
         * for the orchestrator's independent consensus check. */
        unsigned int dig = fnv1a(rout.winner, rout.winner_len);
        snprintf(line, sizeof(line),
                 "AGENTSUB: recalled exact phrase from own noisy probe, write narrowed "
                 "(pid=%u digest=%08x)",
                 self, dig);
        write_str(line);
        char ack[12];
        snprintf(ack, sizeof(ack), "p%08x", dig);
        send_msg(ack, 9);
    } else {
        snprintf(line, sizeof(line), "AGENTSUB BROKEN recall=%d content_ok=%d imprint=%d", (int)r,
                 content_ok, (int)w);
        write_str(line);
    }

    /* One-shot proof: exit so this citizen leaves the ready queue (see agentd). */
    exit_(0);
    for (;;) {
        yield();
    }
}
