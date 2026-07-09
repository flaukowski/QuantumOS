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

/* One UDP datagram: magic + this node's 256 phase bytes. 260 bytes. */
typedef struct {
    uint32_t magic;
    uint8_t phase[GHOST_N];
} fsyn_frame_t;

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
            /* not our reply — ignore and keep waiting */
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

        /* Drain any peer frames and fold them into ghostd. */
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
            if (n < (long)sizeof(fsyn_frame_t)) {
                break; /* WOULDBLOCK or a runt */
            }
            const fsyn_frame_t *f = (const fsyn_frame_t *)rbuf;
            if (f->magic != FSYN_MAGIC) {
                continue; /* not ours — drop */
            }
            uint32_t src = (uint32_t)r.ip[0] | ((uint32_t)r.ip[1] << 8) |
                           ((uint32_t)r.ip[2] << 16) | ((uint32_t)r.ip[3] << 24);
            /* Only fold frames from a CONFIGURED peer — a forged/stray source
             * on the shared L2 (or our own echo) never reaches ghostd's slots. */
            if (!in_peer_set(src)) {
                continue;
            }
            ghost_couple(f->phase, src);
            log_ip("FIELDSYNC: frame from ", src);
        }

        /* ~1 Hz: snapshot our field and send it to EACH peer (N-1 unicasts on
         * the shared L2 — the mean field is all-to-all). */
        if (ticks() - last_send >= 100) {
            last_send = ticks();
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
