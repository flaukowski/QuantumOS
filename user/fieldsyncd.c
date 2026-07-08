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

static uint8_t peer_ip[4];

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

/* Forward the peer's phases into ghostd (GHOST_COUPLE, no reply). */
static void ghost_couple(const uint8_t *phase) {
    ghost_wide_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = 0;
    }
    req.op = GHOST_COUPLE;
    for (int i = 0; i < GHOST_N; i++) {
        req.phase[i] = phase[i];
    }
    send_msg((const char *)&req, (long)sizeof(req));
}

void _start(void) {
    uint32_t pk = (uint32_t)sysinfo(SYSINFO_PEER, 0, 0);
    if (pk == 0) {
        write_str("FIELDSYNC: no peer configured — idle");
        for (;;) {
            heartbeat();
            yield();
        }
    }
    peer_ip[0] = (uint8_t)pk;
    peer_ip[1] = (uint8_t)(pk >> 8);
    peer_ip[2] = (uint8_t)(pk >> 16);
    peer_ip[3] = (uint8_t)(pk >> 24);

    {
        char b[80];
        int o = ghost_put(b, 0, "FIELDSYNC: coupling to peer ");
        for (int i = 0; i < 4; i++) {
            o = ghost_put_u(b, o, peer_ip[i]);
            if (i < 3) {
                b[o++] = '.';
            }
        }
        b[o] = 0;
        write_str(b);
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
            ghost_couple(f->phase);
            char b[80];
            int o = ghost_put(b, 0, "FIELDSYNC: frame from ");
            for (int i = 0; i < 4; i++) {
                o = ghost_put_u(b, o, r.ip[i]);
                if (i < 3) {
                    b[o++] = '.';
                }
            }
            b[o] = 0;
            write_str(b);
        }

        /* ~1 Hz: snapshot our field and send it to the peer. */
        if (ticks() - last_send >= 100) {
            last_send = ticks();
            fsyn_frame_t out;
            out.magic = FSYN_MAGIC;
            if (ghost_snapshot(out.phase)) {
                udp_req_t s;
                for (unsigned i = 0; i < sizeof(s); i++) {
                    ((unsigned char *)&s)[i] = 0;
                }
                s.sock = sock;
                for (int i = 0; i < 4; i++) {
                    s.ip[i] = peer_ip[i];
                }
                s.port = FSYN_PORT;
                s.buf = &out;
                s.len = sizeof(out);
                udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — next round retries */
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
