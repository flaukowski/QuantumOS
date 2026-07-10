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
 *                 cap to a sub-agent (agentsub) via SYS_CAP_DERIVE over a
 *                 capability-checked IPC pair; the sub-agent recalls with it and
 *                 acks. This is "an agent hands a narrowed intent to a sub-agent."
 *
 * On all four it prints the single CI merge-gate line
 *   AGENTD: DEMO OK qpu+field+spawn+delegate
 * Grants: qpu_submit(quota) + field(region 3) + field_delegable + spawn. NOT
 * monitored (a watchdog respawn would re-run the one-shot proof); idles after.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "usys.h"
#include "qpu_circuit.h"

#define AGENT_REGION 3
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
static int do_delegate(void) {
    char buf[16];
    long sub = 0;
    for (long spins = 0; spins < 8000000L && !sub; spins++) {
        long s = recv_msg(buf, sizeof(buf));
        if (s != 0) {
            sub = s; /* the kernel vouches the sender pid */
        } else {
            yield();
        }
    }
    if (!sub) {
        write_str("AGENT BROKEN delegate no sub ready");
        return 0;
    }

    cap_derive_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    req.resource_type = CAP_RESOURCE_FIELD;
    req.resource_id = AGENT_REGION;
    req.permissions = CAP_READ; /* narrowed: no WRITE, no GRANT */
    req.target_pid = (unsigned)sub;
    req.expiration = 0;
    long d = cap_derive_(&req);
    if (d != 0) {
        printf("AGENT BROKEN cap_derive=%ld\n", d);
        return 0;
    }
    send_msg("go", 2); /* the cap is ready — the sub-agent may recall now */

    int proven = 0;
    for (long spins = 0; spins < 40000000L && !proven; spins++) {
        if (recv_msg(buf, sizeof(buf)) != 0 && buf[0] == 'p') {
            proven = 1;
        } else {
            yield();
        }
    }
    if (!proven) {
        write_str("AGENT BROKEN delegate no proven ack");
        return 0;
    }
    printf("AGENT: delegated region 3 (READ) to sub=%ld\n", sub);
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
        write_str("AGENTD: DEMO OK qpu+field+spawn+delegate");
    } else {
        printf("AGENTD: DEMO BROKEN qpu=%d field=%d spawn=%d deleg=%d\n", q, f, s, d);
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
