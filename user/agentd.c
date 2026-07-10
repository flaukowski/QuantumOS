/**
 * QuantumOS agentd — the agent-native end-to-end demo (the mission showcase).
 *
 * One ring-3 citizen that exercises the whole agentic stack in a single story,
 * each step with a verifiable outcome (agentd carries no engine of its own — the
 * results can only have come back through the kernel's brokers):
 *
 *   1. QPU      — submits a Bell circuit through the SYS_QPU broker and checks
 *                 the EXACT probability qpud computed, p(00) = 1/2.
 *   2. FIELD    — imprints a phrase into holographic field region 3 and recalls
 *                 it from a deterministically CORRUPTED probe (wave-interference
 *                 associative memory), verifying the exact stored bytes return.
 *   3. SPAWN    — starts /bin/hello as a sub-process and waits for it to exit
 *                 (real spawn authority — only a citizen holding the cap can).
 *   4. DELEGATE — hands a strictly-NARROWED READ-only slice of its region-3 field
 *                 cap to a SOCIETY of AGENT_SUBS sub-agents (agentsub) via
 *                 SYS_CAP_DERIVE, each over its own capability-checked IPC pair;
 *                 every sub-agent recalls with its cap and acks. This is "an
 *                 orchestrator hands a narrowed intent to each of several
 *                 sub-agents" — a one-hop fan-out tree (CAP_GRANT never handed
 *                 over, so no sub can re-delegate).
 *
 * On all four it prints the single CI merge-gate line
 *   AGENTD: DEMO OK qpu+field+spawn+society
 * Grants: qpu_submit(quota) + field(region 3) + field_delegable + spawn. NOT
 * monitored (a watchdog respawn would re-run the one-shot proof); exits after.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "usys.h"
#include "qpu_circuit.h"

#define AGENT_REGION 3
#define AGENT_SUBS 3 /* the society: agentd delegates to this many sub-agents */
static const unsigned char AGENT_PHRASE[] = "agentd end to end field phrase";
#define AGENT_PHRASE_LEN (sizeof(AGENT_PHRASE) - 1)

static unsigned int get_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

/* Submit an opaque circuit and poll it to completion (the qpu_test pattern).
 * Returns the job id (>0) with the DONE result in *out, or <=0 on failure. */
static long submit_and_wait(const unsigned char *circuit, unsigned int len, qpu_poll_out_t *out) {
    qpu_submit_req_t req;
    req.circuit_len = len;
    for (unsigned int i = 0; i < len; i++) {
        req.circuit[i] = circuit[i];
    }
    long jid = qpu_submit_(&req);
    if (jid <= 0) {
        return jid;
    }
    for (int spins = 0; spins < 2000000; spins++) {
        long st = qpu_poll_(jid, out);
        if (st == QPU_POLL_DONE) {
            return jid;
        }
        if (st < 0) {
            return st;
        }
        yield();
    }
    return -100; /* timed out waiting for qpud */
}

/* Step 1 — a Bell pair through the broker: p(00) must be exactly 1/2. */
static int do_qpu(void) {
    unsigned char bell[QC_HDR + 2 * 3];
    bell[0] = QC_VERSION;
    bell[1] = 2; /* qubits */
    bell[2] = 2; /* ops */
    bell[3] = 0; /* probe |00> */
    bell[4] = 0;
    bell[5] = bell[6] = bell[7] = 0;
    bell[8] = QC_OP_H;
    bell[9] = 0;
    bell[10] = 0;
    bell[11] = QC_OP_CNOT;
    bell[12] = 0;
    bell[13] = 1;
    qpu_poll_out_t out;
    long j = submit_and_wait(bell, sizeof(bell), &out);
    if (j <= 0 || out.status != QPU_STATUS_OK || out.result_len != QC_RESULT_LEN) {
        printf("AGENT BROKEN qpu submit/poll j=%ld\n", j);
        return 0;
    }
    unsigned int num = get_u32(out.result + 4);
    unsigned int den = get_u32(out.result + 8);
    if (num == 1 && den == 2) {
        printf("AGENT: QPU bell via broker job=%ld p=1/2 (exact)\n", j);
        return 1;
    }
    printf("AGENT BROKEN qpu p=%u/%u\n", num, den);
    return 0;
}

/* Step 2 — imprint region 3, then recall it from a corrupted probe. */
static int do_field(void) {
    field_imprint_req_t ireq;
    for (unsigned i = 0; i < sizeof(ireq); i++) {
        ((unsigned char *)&ireq)[i] = 0;
    }
    ireq.region = AGENT_REGION;
    ireq.len = AGENT_PHRASE_LEN;
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        ireq.pattern[n] = AGENT_PHRASE[n];
    }
    if (imprint_(&ireq) < 0) {
        write_str("AGENT BROKEN field imprint");
        return 0;
    }

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
    rreq.probe[1] ^= 0x20; /* corrupt two bytes — the probe is deliberately noisy */
    rreq.probe[5] ^= 0x20;
    if (recall_(&rreq, &rout) != 0 || rout.winner_len != AGENT_PHRASE_LEN) {
        write_str("AGENT BROKEN field recall");
        return 0;
    }
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        if (rout.winner[n] != AGENT_PHRASE[n]) {
            write_str("AGENT BROKEN field winner mismatch");
            return 0;
        }
    }
    write_str("AGENT: field recalled the phrase from a noisy probe");
    return 1;
}

/* Step 3 — spawn /bin/hello and wait for it to exit. */
static int do_spawn(void) {
    long pid = spawn_("/bin/hello");
    if (pid <= 0) {
        printf("AGENT BROKEN spawn=%ld\n", pid);
        return 0;
    }
    for (long spins = 0; spins < 8000000L; spins++) {
        long w = waitpid_(pid);
        if (w != WAITPID_RUNNING) {
            printf("AGENT: spawned /bin/hello pid=%ld exit=%ld\n", pid, w);
            return 1;
        }
        yield();
    }
    write_str("AGENT BROKEN spawn waitpid timeout");
    return 0;
}

/* Step 4 — delegate a narrowed READ cap over region 3 to agentsub. */
/* Step 4 — the SOCIETY: delegate a narrowed READ slice of region 3 to EACH of a
 * society of AGENT_SUBS sub-agents (a one-hop fan-out tree — CAP_GRANT is never
 * handed over, so no sub can re-delegate), then collect every one's "proven" ack.
 * Each sub-agent announced itself with a "ready" whose kernel-vouched sender pid
 * is how we address it; cap_derive requires that IPC-peer relationship (the
 * kernel wired one per sub). */
static int do_delegate(void) {
    char buf[16];
    long subs[AGENT_SUBS];
    int n = 0;

    /* Collect the sub-agents' pids from their authentic "ready" messages. */
    for (long spins = 0; spins < 20000000L && n < AGENT_SUBS; spins++) {
        long s = recv_msg(buf, sizeof(buf));
        if (s != 0 && buf[0] == 'r') {
            int dup = 0;
            for (int i = 0; i < n; i++) {
                if (subs[i] == s) {
                    dup = 1;
                }
            }
            if (!dup) {
                subs[n++] = s;
            }
        } else {
            yield();
        }
    }
    if (n < AGENT_SUBS) {
        printf("AGENT BROKEN society only %d/%d ready\n", n, AGENT_SUBS);
        return 0;
    }

    /* Delegate to each sub-agent, then release it with an ADDRESSED "go". */
    for (int i = 0; i < AGENT_SUBS; i++) {
        cap_derive_req_t req;
        for (unsigned b = 0; b < sizeof(req); b++) {
            ((unsigned char *)&req)[b] = 0;
        }
        req.resource_type = CAP_RESOURCE_FIELD;
        req.resource_id = AGENT_REGION;
        req.permissions = CAP_READ; /* narrowed: no WRITE, no GRANT */
        req.target_pid = (unsigned)subs[i];
        req.expiration = 0;
        long d = cap_derive_(&req);
        if (d != 0) {
            printf("AGENT BROKEN cap_derive[%d]=%ld\n", i, d);
            return 0;
        }
        send_to(subs[i], "go", 2);
    }

    /* Collect every sub-agent's "proven" ack (it recalled with the delegated cap). */
    int proven = 0;
    for (long spins = 0; spins < 60000000L && proven < AGENT_SUBS; spins++) {
        if (recv_msg(buf, sizeof(buf)) != 0 && buf[0] == 'p') {
            proven++;
        } else {
            yield();
        }
    }
    if (proven < AGENT_SUBS) {
        printf("AGENT BROKEN society only %d/%d proven\n", proven, AGENT_SUBS);
        return 0;
    }
    printf("AGENT: delegated region 3 (READ) to a society of %d sub-agents\n", AGENT_SUBS);
    return 1;
}

void _start(void) {
    /* Region 3 is imprinted in do_field() BEFORE do_delegate() hands the cap
     * over, so the sub-agent's recall has content. do_qpu/do_spawn yield, giving
     * the sub-agent time to send its buffered "ready". */
    int q = do_qpu();
    int f = do_field();
    int s = do_spawn();
    int d = do_delegate();

    if (q && f && s && d) {
        write_str("AGENTD: DEMO OK qpu+field+spawn+society");
    } else {
        printf("AGENTD: DEMO BROKEN qpu=%d field=%d spawn=%d society=%d\n", q, f, s, d);
    }
    /* One-shot proof: EXIT rather than idle-spin. A TERMINATED process leaves the
     * scheduler's ready queue, so the demo does not keep the idle-loop reaper (and
     * every other citizen's death cleanup) starved by an ever-ready yield loop.
     * Unmonitored, so it is reaped, not restarted; its caps cascade-revoke. */
    exit_(0);
    for (;;) {
        yield();
    }
}
