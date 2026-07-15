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

#include "ghost.h"  /* ghost_wide_t, GHOST_SNAPSHOT/COUPLE, string builders */
#include "sha256.h" /* hmac_sha256 + constant-time compare (ADR-0019) */
#include "fsyn.h"   /* fsyn_frame_t/fsyp_frame_t + FSYN_* (frozen wire, ADR-0020) */

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

/* The host-admitted swarm-plane group session key (ADR-0019). Delivered from
 * swarm_svc (the sole COM2 holder) over IPC as "K"+32 bytes. Held in memory: a
 * watchdog rebirth loses it, and since ADR-0023 swarm_svc RE-FORWARDS its
 * cached copy when it observes our pid change (the declarative re-mint
 * restores the delivery path; the re-forward restores the key — without it a
 * reborn fieldsyncd would be keyless-but-wired, emitting zero-tag frames its
 * keyed peers reject). Until it (re)arrives, the wire is unauthenticated
 * exactly as before — increment B-frame-auth makes possession of this key
 * load-bearing. */
static uint8_t session_key[32];
static int have_key;
/* Monotonic per-sender transmit sequence, seeded from the boot tick at _start
 * (and re-seeded on rebirth) so a reborn node's seq is always above what its
 * peers last accepted — SYS_TICKS survives ring-3 rebirth and climbs ~100/s.
 * A TRANSMIT counter, not content-derived: each send cycle bumps it. */
static uint32_t tx_seq;

/* ADR-0019 replay watermark: the highest FSYN seq accepted from each peer.
 * Per-peer so one peer's stream can't advance another's; reset on a (re)key. */
static uint32_t peer_last_seq[GHOST_MAX_PEERS];
/* Saturating count of frames rejected for a bad MAC or a stale seq — one
 * console line at the first reject; the counter never wraps or spams. */
static uint32_t rejects;

/* Capture a "K"+32-byte key handoff from swarm_svc. Dual-sited like
 * note_aggregate — the key can race a snapshot reply. `got` is recv_msg's
 * return, which is the SENDER PID (non-zero) on a message, 0 on an empty queue
 * — NOT a byte count — so the message is identified by its 'K' tag, not length.
 * swarm_svc always sends exactly "K"+32 bytes, and the caller's buffer is
 * zero-initialized and >= 33 bytes, so the 32-byte read is always in bounds.
 * 'K' (0x4B) cannot collide with agentd's 'A' or ghostd's small op bytes. */
static void note_key(const char *buf, long got) {
    if (got != 0 && buf[0] == 'K') {
        for (int i = 0; i < 32; i++) {
            session_key[i] = (uint8_t)buf[i + 1];
        }
        /* A (re)key restarts the sequence space: clear every peer's replay
         * watermark so the first frame under the new key is accepted. */
        for (int i = 0; i < GHOST_MAX_PEERS; i++) {
            peer_last_seq[i] = 0;
        }
        if (!have_key) {
            write_str("FSKEY: swarm-plane session key installed");
        }
        have_key = 1;
    }
}

/* The configured peer set (epic #139 N-way society): up to GHOST_MAX_PEERS
 * packed IPv4 addresses read from SYSINFO_PEER_COUNT / SYSINFO_PEER<index>. A
 * 2-VM boot has exactly one. NOTE: this is the packed IP, not a key — source-IP
 * is the ONLY admission check today (the ADR-0019 threat surface). */
static uint32_t peer_ip[GHOST_MAX_PEERS];
static int peer_count;

/* Record a rejected frame: log the reason ONCE (the first reject), then bump a
 * saturating counter — never a per-frame print (a flood must not spam) and
 * never a kernel-ledger write (ring 3 cannot forge the authority ledger). */
static void note_reject(const char *why) {
    if (rejects == 0) {
        write_str(why);
    }
    if (rejects != 0xffffffffu) {
        rejects++;
    }
}

/* Which configured peer is this source IP? Returns the peer INDEX (so the
 * caller can select peer_last_seq[i]) or -1 for a forged/stray/self source.
 * Source-IP alone is spoofable on a shared L2 — the per-frame MAC is the real
 * ADR-0019 admission check. */
static int in_peer_set(uint32_t ip) {
    for (int i = 0; i < peer_count; i++) {
        if (peer_ip[i] == ip) {
            return i;
        }
    }
    return -1;
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
            /* Not our reply. An agentd aggregate handoff OR a swarm_svc key
             * handoff can race the snapshot reply — capture it here rather
             * than dropping it. */
            note_aggregate(buf, s);
            note_key(buf, s);
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
        peer_ip[i] = (uint32_t)sysinfo(SYSINFO_PEER, (void *)(long)i, 0);
        log_ip("FIELDSYNC: coupling to peer ", peer_ip[i]);
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
    /* Seed the transmit sequence above any plausible peer watermark (ADR-0019):
     * boot-relative ticks, so even a reborn node never restarts seq at 0. */
    tx_seq = (uint32_t)ticks();

    for (;;) {
        heartbeat();

        /* Drain a pending agentd aggregate handoff (epic #178). Non-blocking;
         * ghost snapshot replies arriving here are impossible (each snapshot
         * request is awaited to completion before the loop continues). */
        {
            char abuf[40]; /* fits "A"+8hex (agentd) and "K"+32-byte key (swarm) */
            for (unsigned i = 0; i < sizeof(abuf); i++) {
                abuf[i] = 0;
            }
            long got = recv_msg(abuf, sizeof(abuf));
            note_aggregate(abuf, got);
            note_key(abuf, got);
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
            int pidx = in_peer_set(src);
            if (pidx < 0) {
                continue;
            }
            if (magic == FSYN_MAGIC && n >= (long)sizeof(fsyn_frame_t)) {
                const fsyn_frame_t *f = (const fsyn_frame_t *)rbuf;
                /* ADR-0019: once keyed, a frame must carry a valid HMAC over
                 * magic||seq||phase AND a strictly-newer seq (replay guard). An
                 * unkeyed node accepts as before — the wire grew, but coupling
                 * is unchanged until the host admits a key. Source IP already
                 * passed in_peer_set; the MAC is what a spoofed source cannot
                 * forge. */
                if (have_key) {
                    uint8_t want[32];
                    hmac_sha256(session_key, 32, rbuf, FSYN_MAC_COVERED, want);
                    if (!hmac_sha256_equal(want, f->tag)) {
                        note_reject("FSAUTH: forged FSYN frame rejected (bad MAC)");
                        continue;
                    }
                    if (f->seq <= peer_last_seq[pidx]) {
                        note_reject("FSAUTH: stale FSYN frame rejected (replay)");
                        continue;
                    }
                    peer_last_seq[pidx] = f->seq;
                }
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
                        s.ip[i] = (uint8_t)((peer_ip[pi] >> (8 * i)) & 0xFF);
                    }
                    s.port = FSYN_PORT;
                    s.buf = &pout;
                    s.len = sizeof(pout);
                    udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — next round retries */
                }
            }
            fsyn_frame_t out;
            for (unsigned i = 0; i < sizeof(out); i++) {
                ((uint8_t *)&out)[i] = 0; /* seq=0 + zero tag for the unkeyed path */
            }
            out.magic = FSYN_MAGIC;
            if (ghost_snapshot(out.phase)) {
                wiring_warned = 0; /* wiring works — a LATER outage warns anew */
                /* ADR-0019: a keyed node stamps a fresh transmit seq and an HMAC
                 * over magic||seq||phase; an unkeyed node ships seq=0 + a zero
                 * tag (which a keyed peer rejects and an unkeyed peer ignores). */
                if (have_key) {
                    out.seq = ++tx_seq;
                    hmac_sha256(session_key, 32, (const uint8_t *)&out, FSYN_MAC_COVERED, out.tag);
                }
                for (int pi = 0; pi < peer_count; pi++) {
                    udp_req_t s;
                    for (unsigned i = 0; i < sizeof(s); i++) {
                        ((unsigned char *)&s)[i] = 0;
                    }
                    s.sock = sock;
                    for (int i = 0; i < 4; i++) {
                        s.ip[i] = (uint8_t)((peer_ip[pi] >> (8 * i)) & 0xFF);
                    }
                    s.port = FSYN_PORT;
                    s.buf = &out;
                    s.len = sizeof(out);
                    udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — next round retries */
                }
            } else if (!wiring_warned) {
                /* send_msg to ghostd failed. Since ADR-0023 the pair is
                 * re-minted declaratively on every restart of either end, so
                 * this is TRANSIENT (ghostd between death and rebirth, its
                 * stale cap already unlinked) — or a real mint failure. Say
                 * so once per outage rather than spin silently; the next
                 * snapshot round retries and a success re-arms the warning. */
                write_str("FIELDSYNC: ghostd snapshot unavailable (peer down or mint failed)");
                wiring_warned = 1;
            }
        }
        yield();
    }
}
