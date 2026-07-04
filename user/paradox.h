/**
 * QuantumOS paradoxd — shared protocol + fixed-point definitions
 * (ghostOS phase 3, #50).
 *
 * paradoxd is a paradox-resolution service: it iterates a contraction rule
 * on a Q16.16 state until a successive-delta convergence test passes (or an
 * iteration budget is hit), then reports how many steps it took. It runs a
 * CONVERGENT <-> DIVERGENT phase machine whose transitions are gated on the
 * order parameter R of ghostd's Hopfield-Kuramoto field, queried over
 * capability-checked IPC. Two services, coupled through the field.
 *
 * ALL arithmetic is integer / fixed-point Q16.16 — no floats, no doubles,
 * anywhere (the reciprocal used by the "x = 1/x" rule is an integer divide
 * of ONE*ONE by the state, rounded). A full "1.0" is Q16_ONE = 65536.
 *
 * This header carries only what the service (paradoxd) and its boot client
 * (paradox-test) must agree on: the wire request and the deterministic
 * canned contradiction the merge gate resolves.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PARADOX_H
#define PARADOX_H

#include <stdint.h>
#include "usys.h"

/* Q16.16 fixed point: 1.0 == 65536. */
#define Q16_SHIFT 16
#define Q16_ONE 65536

/* State-vector length for the consensus (row-stochastic) rule. */
#define PARADOX_VK 4

/* IPC opcode (request .op). Chosen distinct from any ghost_rep_t .op (which
 * echoes GHOST_REMEMBER/RECALL/STATUS = 1/2/3) so paradoxd can tell a client
 * contradiction apart from a ghostd STATUS reply landing in the same mailbox. */
#define PARADOX_RESOLVE 0xA1

/* Wire request: a canned contradiction to resolve. 24 bytes, far below
 * IPC_MAX_MESSAGE_SIZE. `x0` seeds the scalar reciprocal-average rule (the
 * classic x = 1/x paradox); `vec` seeds the vector consensus rule. */
typedef struct {
    uint8_t op; /* PARADOX_RESOLVE */
    uint8_t pad[3];
    int32_t x0;              /* Q16.16 scalar seed (nonzero) */
    int32_t vec[PARADOX_VK]; /* Q16.16 vector seed */
} paradox_req_t;

/* The deterministic contradiction the merge gate hands paradoxd. Fixed so
 * the whole gate path is randomness-free: x0 = 8.0 (well away from the +-1
 * attractor), a spread vector for the consensus rule. */
#define PARADOX_GATE_X0 (8 * Q16_ONE)  /* 8.0 */
#define PARADOX_GATE_V0 (4 * Q16_ONE)  /*  4.0 */
#define PARADOX_GATE_V1 (-2 * Q16_ONE) /* -2.0 */
#define PARADOX_GATE_V2 (6 * Q16_ONE)  /*  6.0 */
#define PARADOX_GATE_V3 (0)            /*  0.0 */

#endif /* PARADOX_H */
