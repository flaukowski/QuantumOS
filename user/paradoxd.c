/**
 * QuantumOS paradoxd — a fixed-point paradox-resolution service running as
 * an isolated ring-3 user process (ghostOS phase 3, #50; epic #47).
 *
 * paradoxd resolves "contradictions" by damped iteration to an attractor —
 * the same idea as NickFlach/ParadoxResolver's fixed-point resolver, done
 * here in pure Q16.16 integer arithmetic (no floats anywhere). It ships two
 * contraction rules:
 *
 *   (a) Scalar reciprocal-average — the classic x = 1/x paradox:
 *           x_{n+1} = (x_n + ONE*ONE/x_n) / 2
 *       a contraction whose fixed points solve x = 1/x, i.e. x* = +-1. The
 *       reciprocal ONE*ONE/x is an integer divide (rounded), never a float.
 *       Positive seeds converge to +1, negative to -1. This is the rule the
 *       merge gate resolves.
 *
 *   (b) Vector consensus (row-stochastic averaging) — a circular smoothing
 *           v_i <- (v_{i-1} + 2 v_i + v_{i+1}) / 4
 *       whose transition matrix is doubly stochastic, so it preserves the
 *       component sum and contracts every vector toward its balanced fixed
 *       point: the uniform vector whose entries all equal the mean. paradoxd
 *       runs this on its internal state during CONVERGENT phases.
 *
 * Convergence for both is a successive-delta test: stop once the largest
 * per-step change falls below EPSILON, or the iteration budget is spent.
 *
 * Phase machine (the coupling — the point of phase 3): paradoxd cycles
 * CONVERGENT <-> DIVERGENT. CONVERGENT phases contract (rule b); DIVERGENT
 * phases inject a bounded perturbation from paradoxd's internal PRNG. The
 * transitions are *gated on ghostd's field*: paradoxd holds an IPC cap for
 * ghostd, polls its STATUS order parameter R, and is allowed to enter
 * CONVERGENT only when R is high (field coherent) and DIVERGENT only when R
 * is low (field incoherent). It prints the R it saw at each transition.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "paradox.h"
#include "ghost.h" /* ghost_req_t/ghost_rep_t, GHOST_STATUS, string builders */

/* ---- resolver constants ---- */
#define PARADOX_EPS 4                 /* Q16 successive-delta convergence floor (~6e-5) */
#define PARADOX_MAX_ITERS 64          /* iteration budget per resolve */
#define PARADOX_VCLAMP (16 * Q16_ONE) /* keep vector components bounded */

/* ---- phase-transition thresholds, gated on ghostd's R (Q16) ----
 * Measured, not guessed: after the 3/3 recall ghostd's field locks into an
 * attractor and its order parameter reads a *steady* R = 63920 (0.9754) on
 * every STATUS poll — the lambda-damp band [0.80, 0.98] never trips at that
 * value, so the field neither sheds nor re-tightens and R does not drift.
 * paradoxd therefore cannot rely on R swinging to drive it; instead its own
 * dynamics propose each transition (contract until converged -> want to
 * diverge; perturb for a bounded run -> want to re-converge) and R *gates*
 * whether the proposal is allowed. The thresholds bracket the measured R so
 * a coherent field permits the cycle: a re-lock (-> CONVERGENT) needs
 * R >= 0.95, an exploration (-> DIVERGENT) needs R < 0.99. Were the field to
 * fall below 0.95 paradoxd would be held DIVERGENT (cannot re-lock); above
 * 0.99 it would be pinned CONVERGENT. */
#define PARADOX_R_CONVERGE 62259u /* 0.95: R at/above allows -> CONVERGENT */
#define PARADOX_R_DIVERGE 64881u  /* 0.99: R below    allows -> DIVERGENT */

/* Bounded length of a DIVERGENT exploration before paradoxd wants to
 * re-converge (perturbation steps). */
#define PARADOX_DIV_STEPS 16

/* ---- ghost poll cadence ---- */
#define PARADOX_POLL_IDLE 16     /* idle iterations between STATUS queries */
#define PARADOX_STEPS_PER_TICK 8 /* resolver steps per scheduled quantum */

/* Set to 1 to trace every observed R while tuning thresholds. */
#define PARADOX_TRACE_R 0

enum { PHASE_CONVERGENT = 0, PHASE_DIVERGENT = 1 };

/* ---- state (zeroed .bss) ---- */
static int phase = PHASE_CONVERGENT;
static int32_t state_vec[PARADOX_VK];
static uint32_t prng_state32 = 0x9E3779B9u;
static int awaiting_ghost = 0;
static int converged = 0;   /* CONVERGENT state has reached consensus */
static int div_steps = 0;   /* perturbation steps taken this DIVERGENT run */
static uint32_t last_R = 0; /* most recent ghostd order parameter (Q16) */
static int have_R = 0;      /* a real R has been received at least once */

static void logline(const char *s) {
    write_str(s);
}

/* Internal xorshift32 — the divergent-phase perturbation source. */
static uint32_t prng32(void) {
    uint32_t x = prng_state32;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state32 = x ? x : 0x1u;
    return prng_state32;
}

/* Rounded integer reciprocal in Q16.16: returns ONE*ONE / x (i.e. 1/x in
 * Q16), rounded to nearest. x must be nonzero. ONE*ONE = 2^32 fits int64. */
static int32_t q16_recip(int32_t x) {
    int64_t num = (int64_t)Q16_ONE * (int64_t)Q16_ONE; /* 2^32 */
    int64_t half = (x > 0) ? (x / 2) : -((-x) / 2);    /* round to nearest */
    return (int32_t)((num + half) / x);
}

/* Rule (a): resolve x = 1/x by damped reciprocal-average. Returns the
 * settled value (~ +-ONE) and writes the iteration count to *iters. */
static int32_t resolve_scalar(int32_t x0, int *iters) {
    int32_t x = x0 ? x0 : 1; /* a zero seed is a degenerate contradiction */
    int it = 0;
    for (; it < PARADOX_MAX_ITERS; it++) {
        int32_t r = q16_recip(x);
        int32_t nx = (int32_t)(((int64_t)x + (int64_t)r) / 2);
        int32_t d = nx - x;
        if (d < 0)
            d = -d;
        x = nx;
        if (d < PARADOX_EPS) {
            it++;
            break;
        }
    }
    *iters = it;
    return x;
}

/* Rule (b): one circular row-stochastic smoothing step over state_vec.
 * Returns the largest per-component change (Q16) for the convergence test. */
static int32_t step_vector(void) {
    int32_t nv[PARADOX_VK];
    for (int i = 0; i < PARADOX_VK; i++) {
        int32_t l = state_vec[(i + PARADOX_VK - 1) % PARADOX_VK];
        int32_t c = state_vec[i];
        int32_t r = state_vec[(i + 1) % PARADOX_VK];
        nv[i] = (int32_t)(((int64_t)l + 2 * (int64_t)c + (int64_t)r) / 4);
    }
    int32_t maxd = 0;
    for (int i = 0; i < PARADOX_VK; i++) {
        int32_t d = nv[i] - state_vec[i];
        if (d < 0)
            d = -d;
        if (d > maxd)
            maxd = d;
        state_vec[i] = nv[i];
    }
    return maxd;
}

/* DIVERGENT step: inject a bounded perturbation into state_vec from the
 * internal PRNG, clamped so the vector never runs away. */
static void perturb_vector(void) {
    for (int i = 0; i < PARADOX_VK; i++) {
        int32_t nb = (int32_t)(int8_t)(prng32() & 0xFFu); /* [-128, 127] */
        int32_t kick = nb * (Q16_ONE / 128);              /* up to ~+-1.0 */
        int32_t v = state_vec[i] + kick;
        if (v > PARADOX_VCLAMP)
            v = PARADOX_VCLAMP;
        if (v < -PARADOX_VCLAMP)
            v = -PARADOX_VCLAMP;
        state_vec[i] = v;
    }
}

/* Send a STATUS request to ghostd (routed by paradoxd's single IPC send-cap,
 * which addresses ghostd). The reply lands in our own mailbox. */
static void query_ghost(void) {
    ghost_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++)
        ((uint8_t *)&req)[i] = 0;
    req.op = GHOST_STATUS;
    long rc = send_msg((const char *)&req, (long)sizeof(req));
    if (rc == 0) {
        awaiting_ghost = 1;
    }
}

/* Cache the latest order parameter from a ghostd STATUS reply. The phase
 * gate reads this most-recent value, so transitions run at paradoxd's own
 * step cadence rather than stalling on an IPC round-trip each time. */
static void note_ghost_R(uint32_t r_q16) {
    last_R = r_q16;
    have_R = 1;
#if PARADOX_TRACE_R
    {
        char b[48];
        int o = ghost_put(b, 0, "PARADOXD: ghost Rraw=");
        o = ghost_put_u(b, o, (unsigned)r_q16);
        b[o] = 0;
        logline(b);
    }
#endif
}

/* Announce a phase change together with the R that gated it. */
static void log_transition(void) {
    char b[64];
    int o = ghost_put(b, 0, "PARADOXD: phase -> ");
    o = ghost_put(b, o, phase == PHASE_CONVERGENT ? "CONVERGENT" : "DIVERGENT");
    o = ghost_put(b, o, " (ghost R=");
    o = ghost_put_r(b, o, last_R);
    o = ghost_put(b, o, ")");
    b[o] = 0;
    logline(b);
}

/* One step of paradoxd's own dynamics under the current phase, then let the
 * cached ghostd R gate any transition its dynamics propose (converged ->
 * want to diverge; enough perturbation -> want to re-converge). No proposal
 * is honoured until a real R has been seen. */
static void phase_step(void) {
    if (phase == PHASE_CONVERGENT) {
        int32_t d = step_vector(); /* contract toward consensus */
        if (d < PARADOX_EPS)
            converged = 1; /* reached the fixed point */
        if (have_R && converged && last_R < PARADOX_R_DIVERGE) {
            phase = PHASE_DIVERGENT;
            div_steps = 0;
            log_transition();
        }
    } else {
        perturb_vector(); /* inject bounded divergence */
        div_steps++;
        if (have_R && div_steps >= PARADOX_DIV_STEPS && last_R >= PARADOX_R_CONVERGE) {
            phase = PHASE_CONVERGENT;
            converged = 0;
            log_transition();
        }
    }
}

/* The merge gate: resolve the canned contradiction deterministically and
 * print exactly one gate line. Runs in the initial CONVERGENT phase before
 * any ghost-gated transition, so the line — and its iteration count — is
 * randomness-free and stable. Format (grepped by ci-smoke + CI):
 *
 *     PARADOXD: RESOLVED n=<iters> phase=C
 */
static void resolve_gate(const paradox_req_t *req) {
    int iters = 0;
    (void)resolve_scalar(req->x0, &iters);

    /* Seed the coupling's vector state from the same request. */
    for (int i = 0; i < PARADOX_VK; i++)
        state_vec[i] = req->vec[i];

    char line[48];
    int o = ghost_put(line, 0, "PARADOXD: RESOLVED n=");
    o = ghost_put_u(line, o, (unsigned)iters);
    o = ghost_put(line, o, " phase=C");
    line[o] = 0;
    logline(line);
}

void _start(void) {
    long restarts = svc_restarts();
    if (restarts > 0) {
        logline("PARADOXD: REBORN — resolver restarted by the watchdog");
    } else {
        logline("PARADOXD: online in ring 3 — fixed-point paradox resolver");
    }

    prng_state32 = (uint32_t)ticks() ^ 0x9E3779B9u;
    if (prng_state32 == 0)
        prng_state32 = 1u;

    char buf[sizeof(paradox_req_t) + 8];

    /* Phase A — the gate. Block (heartbeating) for the client's canned
     * contradiction, resolve it in the initial CONVERGENT phase, print the
     * one gate line. Only paradox-test holds a send-cap to us, and it sends
     * exactly this, so it is the first message we can receive. */
    int resolved = 0;
    while (!resolved) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender != 0 && (uint8_t)buf[0] == PARADOX_RESOLVE) {
            resolve_gate((const paradox_req_t *)buf);
            resolved = 1;
        } else {
            heartbeat();
            yield();
        }
    }

    /* Phase B — the coupling. Poll ghostd's field and let its order parameter
     * gate the CONVERGENT <-> DIVERGENT cycle, running our own dynamics as we
     * go. Randomness here (divergent perturbation) never touches the gate line
     * above, which is already printed. */
    long idle = 0;
    while (1) {
        long sender = recv_msg(buf, sizeof(buf));
        if (sender != 0) {
            if ((uint8_t)buf[0] == GHOST_STATUS) {
                const ghost_rep_t *rep = (const ghost_rep_t *)buf;
                awaiting_ghost = 0;
                note_ghost_R(rep->r_q16);
            }
            /* A stray PARADOX_RESOLVE after the gate is ignored: the gate
             * fires exactly once. */
            heartbeat();
            continue;
        }

        /* idle */
        if (!awaiting_ghost && ++idle >= PARADOX_POLL_IDLE) {
            idle = 0;
            query_ghost();
        }
        /* Run a small batch of resolver steps per quantum so the phase cycle
         * turns over promptly under round-robin scheduling; still yields each
         * loop so paradoxd never hogs the CPU. */
        for (int s = 0; s < PARADOX_STEPS_PER_TICK; s++)
            phase_step();
        heartbeat();
        yield();
    }
}
