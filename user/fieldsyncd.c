/**
 * QuantumOS fieldsyncd — couple two ghostd oscillator fields over UDP
 * (epic #97): two kernels dreaming together.
 *
 * fieldsyncd is the network half of the coupling; ghostd owns the field
 * dynamics. Each ~1 s, fieldsyncd asks its local ghostd for a phase
 * SNAPSHOT (an IPC round trip) and sends those 256 phases to the peer
 * over UDP. Every peer datagram it receives it forwards to ghostd as a
 * GHOST_COUPLE message, which folds the remote phases in. Mutual on both
 * nodes, the two fields converge — ghostd measures the cross-node order
 * parameter R_x and prints FIELDSYNC lines as it rises.
 *
 * Capabilities: fieldsyncd holds the network cap (grant_net, for SYS_UDP)
 * and an IPC send-cap to ghostd (the paradoxd<->ghostd precedent). It
 * does NOT verify the peer's identity in-guest — the frames carry a boot
 * identity commitment that the HOST verifier checks; in-guest mutual
 * Lamport verification is a one-time signature and out of scope here.
 *
 * The peer address comes from the `peer=` boot token via SYSINFO_PEER; no
 * peer configured -> fieldsyncd idles quietly and the default boot is
 * unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h" /* ghost_wide_t, GHOST_SNAPSHOT/COUPLE, string builders */

#define FSYN_PORT 4747
#define FSYN_MAGIC 0x4E595346u /* 'FSYN' little-endian */
#define FSYP_MAGIC 0x50595346u /* 'FSYP' little-endian — society aggregate (epic #178) */

/* One UDP datagram: magic + this node's 256 phase bytes. 260 bytes. */
typedef struct {
    uint32_t magic;
    uint8_t phase[GHOST_N];
} fsyn_frame_t;

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

/* The local society's aggregate (from agentd via IPC, "A<8hex>") and the
 * last few DISTINCT peer aggregates already printed (print-once dedup so the
 * ~1 Hz resend does not spam the console). All in-memory: a watchdog rebirth
 * loses them — the same documented limitation as the ghostd IPC wiring. */
static uint32_t local_agg;
static int have_agg;
static uint32_t seen_agg[4];
static int seen_count;

/* Parse 8 lowercase-hex digits; returns 0xffffffff on malformed input. */
static uint32_t hex32(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else {
            return 0xffffffffu;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* Capture an "A<8hex>" aggregate handoff from agentd. Called from BOTH recv
 * sites (the main-loop drain AND ghost_snapshot's reply wait, which would
 * otherwise swallow-and-drop a message that races the snapshot reply). */
static void note_aggregate(const char *buf, long got) {
    if (got != 0 && buf[0] == 'A') {
        uint32_t v = hex32(buf + 1);
        if (v != 0xffffffffu) {
            local_agg = v;
            have_agg = 1;
        }
    }
}

/* The configured peer set (epic #139 N-way society): up to GHOST_MAX_PEERS
 * packed IPs read from SYSINFO_PEER_COUNT / SYSINFO_PEER<index>. A 2-VM boot
 * has exactly one. */
static uint32_t peer_pk[GHOST_MAX_PEERS];
static int peer_count;

/* Is a received frame's source one of our configured peers? Drops forged /
 * stray sources on the shared L2 before they reach ghostd's per-peer slots
 * (closes slot-exhaustion + self-coupling). */
static int in_peer_set(uint32_t pk) {
    for (int i = 0; i < peer_count; i++) {
        if (peer_pk[i] == pk) {
            return 1;
        }
    }
    return 0;
}

/* Ask the local ghostd for a phase snapshot (IPC). Fills phase[256].
 * Returns 1 on a valid reply, 0 otherwise. */
static int ghost_snapshot(uint8_t *phase_out) {
    ghost_wide_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = 0;
    }
    req.op = GHOST_SNAPSHOT;
    if (send_msg((const char *)&req, (long)sizeof(req)) != 0) {
        return 0; /* EPERM/EIO — wiring lost (watchdog rebirth); caller logs */
    }
    /* Await the reply (heartbeating so the watchdog leaves us alone). */
    for (int tries = 0; tries < 200; tries++) {
        char buf[sizeof(ghost_wide_t) + 8];
        long s = recv_msg(buf, sizeof(buf));
        if (s != 0) {
            const ghost_wide_t *rep = (const ghost_wide_t *)buf;
            if ((uint8_t)buf[0] == GHOST_SNAPSHOT) {
                for (int i = 0; i < GHOST_N; i++) {
                    phase_out[i] = rep->phase[i];
                }
                return 1;
            }
            /* Not our reply. An agentd aggregate handoff can race the
             * snapshot reply — capture it here rather than dropping it. */
            note_aggregate(buf, s);
        }
        heartbeat();
        yield();
    }
    return 0;
}

/* Forward one peer's phases into ghostd (GHOST_COUPLE, no reply), tagged with
 * that peer's packed source IP so ghostd keys a per-peer slot and folds the
 * MEAN field over N peers. */
static void ghost_couple(const uint8_t *phase, uint32_t src) {
    ghost_wide_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = 0;
    }
    req.op = GHOST_COUPLE;
    for (int i = 0; i < GHOST_N; i++) {
        req.phase[i] = phase[i];
    }
    req.src = src;
    send_msg((const char *)&req, (long)sizeof(req));
}

/* Print "FIELDSYNC: <label> A.B.C.D" for a packed IP. */
static void log_ip(const char *label, uint32_t pk) {
    char b[80];
    int o = ghost_put(b, 0, label);
    for (int i = 0; i < 4; i++) {
        o = ghost_put_u(b, o, (unsigned)((pk >> (8 * i)) & 0xFF));
        if (i < 3) {
            b[o++] = '.';
        }
    }
    b[o] = 0;
    write_str(b);
}

void _start(void) {
    /* Read the configured peer set. SYSINFO_PEER_COUNT is 1 for a 2-VM boot
     * and 0 for a default (no peer=) boot — in which case idle exactly as
     * before (no UDP bind, same message) so every non-society boot is
     * unaffected. */
    peer_count = (int)sysinfo(SYSINFO_PEER_COUNT, 0, 0);
    if (peer_count > GHOST_MAX_PEERS) {
        peer_count = GHOST_MAX_PEERS;
    }
    if (peer_count <= 0) {
        write_str("FIELDSYNC: no peer configured — idle");
        for (;;) {
            heartbeat();
            yield();
        }
    }
    for (int i = 0; i < peer_count; i++) {
        peer_pk[i] = (uint32_t)sysinfo(SYSINFO_PEER, (void *)(long)i, 0);
        log_ip("FIELDSYNC: coupling to peer ", peer_pk[i]);
    }

    udp_req_t bindreq;
    for (unsigned i = 0; i < sizeof(bindreq); i++) {
        ((unsigned char *)&bindreq)[i] = 0;
    }
    bindreq.port = FSYN_PORT;
    long sock = udp_(UDP_BIND, &bindreq);
    if (sock < 0) {
        write_str("FIELDSYNC: UDP bind failed — no coupling");
        for (;;) {
            heartbeat();
            yield();
        }
    }

    long last_send = ticks() - 200; /* send on the first iteration */
    int wiring_warned = 0;

    for (;;) {
        heartbeat();

        /* Drain a pending agentd aggregate handoff (epic #178). Non-blocking;
         * ghost snapshot replies arriving here are impossible (each snapshot
         * request is awaited to completion before the loop continues). */
        {
            char abuf[16];
            for (unsigned i = 0; i < sizeof(abuf); i++) {
                abuf[i] = 0;
            }
            note_aggregate(abuf, recv_msg(abuf, sizeof(abuf)));
        }

        /* Drain any peer frames. Dispatch on MAGIC FIRST, then per-type size:
         * the old `break` on anything under 260 bytes treated a short frame
         * as WOULDBLOCK and stalled the whole drain — but a datagram is
         * CONSUMED by UDP_RECVFROM regardless of its size, so a runt must be
         * `continue`d past, and only a genuinely empty ring (n < 0) ends the
         * drain (the latent demux bug the epic #178 design review found). */
        for (int drain = 0; drain < 8; drain++) {
            udp_req_t r;
            for (unsigned i = 0; i < sizeof(r); i++) {
                ((unsigned char *)&r)[i] = 0;
            }
            static unsigned char rbuf[512];
            r.sock = sock;
            r.buf = rbuf;
            r.len = sizeof(rbuf);
            long n = udp_(UDP_RECVFROM, &r);
            if (n < 0) {
                break; /* WOULDBLOCK — ring empty */
            }
            if (n < 4) {
                continue; /* consumed runt — too short for any magic */
            }
            uint32_t magic = (uint32_t)rbuf[0] | ((uint32_t)rbuf[1] << 8) |
                             ((uint32_t)rbuf[2] << 16) | ((uint32_t)rbuf[3] << 24);
            uint32_t src = (uint32_t)r.ip[0] | ((uint32_t)r.ip[1] << 8) |
                           ((uint32_t)r.ip[2] << 16) | ((uint32_t)r.ip[3] << 24);
            /* Only accept frames from a CONFIGURED peer — a forged/stray
             * source on the shared L2 (or our own echo) never reaches
             * ghostd's slots or the console. */
            if (!in_peer_set(src)) {
                continue;
            }
            if (magic == FSYN_MAGIC && n >= (long)sizeof(fsyn_frame_t)) {
                const fsyn_frame_t *f = (const fsyn_frame_t *)rbuf;
                ghost_couple(f->phase, src);
                log_ip("FIELDSYNC: frame from ", src);
            } else if (magic == FSYP_MAGIC && n >= (long)sizeof(fsyp_frame_t)) {
                /* A peer society's aggregate (epic #178). PRINT once per
                 * distinct value for host-side verification — never imprint:
                 * unauthenticated wire data must not become field content. */
                const fsyp_frame_t *p = (const fsyp_frame_t *)rbuf;
                int dup = 0;
                for (int i = 0; i < seen_count; i++) {
                    if (seen_agg[i] == p->aggregate) {
                        dup = 1;
                    }
                }
                if (!dup) {
                    if (seen_count < 4) {
                        seen_agg[seen_count++] = p->aggregate;
                    }
                    char b[64];
                    int o = ghost_put(b, 0, "FIELDSYNC: peer aggregate a");
                    for (int i = 7; i >= 0; i--) {
                        uint32_t d = (p->aggregate >> (i * 4)) & 0xF;
                        b[o++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    }
                    b[o] = 0;
                    write_str(b);
                }
            }
            /* other magics: not ours — drop */
        }

        /* ~1 Hz: snapshot our field and send it to EACH peer (N-1 unicasts on
         * the shared L2 — the mean field is all-to-all). The society
         * aggregate rides the same cadence as its own fixed 16-byte frame
         * (epic #178): continuous idempotent resend, so a peer that boots
         * later (the society gate boots VMs SEQUENTIALLY) or loses datagrams
         * still converges; receivers dedup by value. */
        if (ticks() - last_send >= 100) {
            last_send = ticks();
            if (have_agg) {
                fsyp_frame_t pout;
                pout.magic = FSYP_MAGIC;
                pout.aggregate = local_agg;
                pout.reserved0 = 0;
                for (int pi = 0; pi < peer_count; pi++) {
                    udp_req_t s;
                    for (unsigned i = 0; i < sizeof(s); i++) {
                        ((unsigned char *)&s)[i] = 0;
                    }
                    s.sock = sock;
                    for (int i = 0; i < 4; i++) {
                        s.ip[i] = (uint8_t)((peer_pk[pi] >> (8 * i)) & 0xFF);
                    }
                    s.port = FSYN_PORT;
                    s.buf = &pout;
                    s.len = sizeof(pout);
                    udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — next round retries */
                }
            }
            fsyn_frame_t out;
            out.magic = FSYN_MAGIC;
            if (ghost_snapshot(out.phase)) {
                for (int pi = 0; pi < peer_count; pi++) {
                    udp_req_t s;
                    for (unsigned i = 0; i < sizeof(s); i++) {
                        ((unsigned char *)&s)[i] = 0;
                    }
                    s.sock = sock;
                    for (int i = 0; i < 4; i++) {
                        s.ip[i] = (uint8_t)((peer_pk[pi] >> (8 * i)) & 0xFF);
                    }
                    s.port = FSYN_PORT;
                    s.buf = &out;
                    s.len = sizeof(out);
                    udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — next round retries */
                }
            } else if (!wiring_warned) {
                /* send_msg to ghostd failed: either no cap (EPERM) or a
                 * stale pid after a watchdog rebirth (EIO). Peer IPC caps
                 * are not re-minted on restart (a known service.c limit);
                 * say so once rather than spin silently. */
                write_str("FIELDSYNC: ghostd wiring lost (known restart limitation)");
                wiring_warned = 1;
            }
        }
        yield();
    }
}
