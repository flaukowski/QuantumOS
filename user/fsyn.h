/**
 * QuantumOS fieldsyncd wire format — the FSYN/FSYP UDP coupling frames
 * (epic #97 two-node coupling; epic #178 society aggregate; ADR-0019
 * authenticated form; frozen as v1 wire contract by ADR-0020 lane C).
 *
 * Moved out of fieldsyncd.c so the wire freeze probe (user/wire_probe.c) can
 * measure the SAME struct layouts the daemon sends — the golden gate diffs
 * compiler-measured sizeof/offsetof, never a size-claim comment.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef FSYN_H
#define FSYN_H

#include "ghost.h" /* GHOST_N (frame geometry); pulls stdint + usys.h */

#define FSYN_PORT 4747
#define FSYN_MAGIC 0x4E595346u /* 'FSYN' little-endian */
#define FSYP_MAGIC 0x50595346u /* 'FSYP' little-endian — society aggregate (epic #178) */

/* One UDP datagram (ADR-0019 authenticated form): magic + a monotonic transmit
 * sequence + this node's 256 phase bytes + an HMAC-SHA256 tag over the leading
 * 264 bytes (magic||seq||phase). 296 bytes. A KEYED node fills seq+tag and a
 * keyed receiver verifies them; an UNKEYED node sends seq=0 + a zero tag and an
 * unkeyed receiver ignores them (backward-compatible coupling — see the recv
 * path). The layout has no padding: 4 + 4 + 256 + 32. */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t phase[GHOST_N];
    uint8_t tag[32];
} fsyn_frame_t;
_Static_assert(sizeof(fsyn_frame_t) == 296, "FSYN wire frame must be 296 bytes");
/* The HMAC covers everything BEFORE the tag: magic(4)+seq(4)+phase(256). */
#define FSYN_MAC_COVERED 264
/* FSYN_MAC_COVERED IS the tag offset by construction — an inserted field that
 * moves the tag without updating the MAC span fails here, in this TU. */
_Static_assert(FSYN_MAC_COVERED == __builtin_offsetof(fsyn_frame_t, tag),
               "HMAC must cover exactly the bytes before the tag");

/* Society-aggregate datagram (epic #178): fixed 16 bytes, sent alongside the
 * phase frame each cycle once the local agentd has handed us its aggregate
 * (continuous idempotent RESEND — the peer VM may boot later, and UDP may
 * drop frames; receivers dedup by value). NEVER imprinted into the field:
 * the design review rejected field-content replication (unauthenticated wire
 * data must not become recallable/persistable field content); received
 * values are only PRINTED for host-side verification. */
typedef struct {
    uint32_t magic;
    uint32_t aggregate;
    uint64_t reserved0;
} fsyp_frame_t;
_Static_assert(sizeof(fsyp_frame_t) == 16, "FSYP wire frame must be 16 bytes");

#endif /* FSYN_H */
