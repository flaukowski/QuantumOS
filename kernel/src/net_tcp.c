/**
 * QuantumOS ring-3 TCP client (epic #82) — split out of net.c.
 *
 * The single active-open TCP connection and its net-thread state machine.
 * All cross-layer wire structs, byte-order helpers and the IPv4/ARP output
 * spine (ip_fill / net_next_hop / resolve_mac) come from net_internal.h;
 * the design notes are on the section comment just below.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net_internal.h>
#include <kernel/interrupts.h> /* timer_get_ticks (TCP timers) */

/* ============================================================================
 * ring-3 TCP client (epic #82)
 *
 * One active-open connection at a time (a single static TCB — the same
 * "one in flight" honesty as the resolver). The IF=1 net thread owns the
 * ENTIRE state machine: all segment TX, timers, and retransmission run
 * there (a syscall can't touch the NIC). Syscalls only read/write shared
 * TCB fields under the publish discipline and post one-shot request flags
 * (connect_req / close_req / abort_req) that the net thread services and
 * clears — independent flags so a reaper's ABORT can never be clobbered
 * by a syscall's CLOSE (the design-attack blocker).
 *
 * Honest scope: client only, stop-and-wait send (one outstanding segment
 * <= MSS), in-order receive only (a segment must start exactly at
 * rcv_nxt; anything else is dropped and the peer retransmits), window =
 * free receive-ring space, short TIME_WAIT (~1s, not 2MSL). No congestion
 * control, no out-of-order reassembly, no options sent but MSS.
 * ============================================================================ */

#define IP_PROTO_TCP 6
#define TCP_MSS 1460
#define TCP_RX_BUF 8192 /* power of two: free-running index & (size-1) */
#define TCP_EPHEMERAL_BASE 49152
#define TCP_RETX_TICKS 50      /* ~0.5s at 100 Hz between (re)transmits */
#define TCP_MAX_RETRIES 8      /* then the connection enters ERROR */
#define TCP_TIMEWAIT_TICKS 100 /* ~1s (documented deviation from 2MSL) */

/* TCP flags. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Connection states. */
#define TCPS_CLOSED 0
#define TCPS_SYN_SENT 1
#define TCPS_ESTABLISHED 2
#define TCPS_FIN_WAIT_1 3
#define TCPS_FIN_WAIT_2 4
#define TCPS_CLOSING 5
#define TCPS_CLOSE_WAIT 6
#define TCPS_LAST_ACK 7
#define TCPS_TIME_WAIT 8
#define TCPS_ERROR 9

/* Signed 32-bit sequence comparison (wrap-safe): a is "after" b iff the
 * signed difference is positive. */
#define SEQ_GT(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)

typedef struct {
    volatile int state;
    uint32_t owner_pid;
    uint8_t rip[4];
    uint16_t rport, lport;
    uint8_t nexthop_mac[ETH_ADDR_LEN];
    int have_mac;

    uint32_t iss;              /* our initial send seq */
    uint32_t snd_una, snd_nxt; /* unacked / next — wrapping */
    uint32_t rcv_nxt;          /* next expected receive seq */

    uint8_t rx[TCP_RX_BUF];
    volatile uint32_t rx_head; /* producer: net thread demux */
    volatile uint32_t rx_tail; /* consumer: RECV syscall */

    uint8_t tx[TCP_MSS];
    uint16_t tx_len;
    volatile int tx_pending; /* syscall stages data + sets 1; net thread clears on ACK */

    int fin_sent;     /* our FIN transmitted (occupies one seq) */
    int fin_received; /* peer FIN consumed */
    uint32_t last_adv_wnd;

    uint32_t retx_tick; /* net thread only: last (re)transmit */
    int retries;
    uint32_t timer_tick; /* net thread only: TIME_WAIT clock */

    volatile int connect_req, close_req, abort_req;
    uint8_t creq_ip[4]; /* pending connect params (written before connect_req) */
    uint16_t creq_port;
} tcp_conn_t;

static tcp_conn_t tcb; /* the one connection */
static uint16_t tcp_ephemeral_next = TCP_EPHEMERAL_BASE;

/* TCP checksum over the pseudo-header ++ segment as ONE running sum
 * (checksum16 finalizes — it cannot be composed across two buffers).
 * Returns 0 for a valid received segment (its stored checksum in place)
 * and the value to store for a transmitted one (checksum field zeroed). */
static uint16_t tcp_cksum(const uint8_t *sip, const uint8_t *dip, const uint8_t *seg,
                          uint16_t seg_len) {
    uint32_t sum = 0;
    sum += (uint32_t)((sip[0] << 8) | sip[1]);
    sum += (uint32_t)((sip[2] << 8) | sip[3]);
    sum += (uint32_t)((dip[0] << 8) | dip[1]);
    sum += (uint32_t)((dip[2] << 8) | dip[3]);
    sum += IP_PROTO_TCP;
    sum += seg_len;
    uint32_t i = 0;
    while (i + 1 < seg_len) {
        sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
        i += 2;
    }
    if (i < seg_len) {
        sum += (uint32_t)(seg[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Free space in the receive ring = the window we advertise. */
static uint32_t tcp_rx_free(void) {
    return TCP_RX_BUF - (tcb.rx_head - tcb.rx_tail);
}

/* Build and transmit one segment (net thread only). `seq` is the sequence
 * number to stamp; `flags` the control bits; `payload`/`plen` the data
 * (<= MSS); `with_mss` adds the MSS option (SYN only, data offset 6). */
static void tcp_xmit(uint32_t seq, uint8_t flags, const uint8_t *payload, uint16_t plen,
                     int with_mss) {
    if (!tcb.have_mac) {
        return;
    }
    static uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 24 + TCP_MSS]; /* net thread only */
    eth_hdr_t *eth = (eth_hdr_t *)out;
    ip_hdr_t *ip = (ip_hdr_t *)(out + sizeof(eth_hdr_t));
    uint8_t *tcp = (uint8_t *)ip + sizeof(ip_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = tcb.nexthop_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    uint8_t doff = with_mss ? 6 : 5;
    uint16_t thlen = (uint16_t)(doff * 4);
    for (uint16_t i = 0; i < thlen; i++) {
        tcp[i] = 0;
    }
    tcp[0] = (uint8_t)(tcb.lport >> 8);
    tcp[1] = (uint8_t)tcb.lport;
    tcp[2] = (uint8_t)(tcb.rport >> 8);
    tcp[3] = (uint8_t)tcb.rport;
    tcp[4] = (uint8_t)(seq >> 24);
    tcp[5] = (uint8_t)(seq >> 16);
    tcp[6] = (uint8_t)(seq >> 8);
    tcp[7] = (uint8_t)seq;
    tcp[8] = (uint8_t)(tcb.rcv_nxt >> 24);
    tcp[9] = (uint8_t)(tcb.rcv_nxt >> 16);
    tcp[10] = (uint8_t)(tcb.rcv_nxt >> 8);
    tcp[11] = (uint8_t)tcb.rcv_nxt;
    tcp[12] = (uint8_t)(doff << 4);
    tcp[13] = flags;
    uint32_t wnd = tcp_rx_free();
    if (wnd > 0xFFFF) {
        wnd = 0xFFFF;
    }
    tcp[14] = (uint8_t)(wnd >> 8);
    tcp[15] = (uint8_t)wnd;
    /* checksum (16,17) and urgent (18,19) already zero */
    if (with_mss) {
        tcp[20] = 2; /* MSS option kind */
        tcp[21] = 4; /* length */
        tcp[22] = (uint8_t)(TCP_MSS >> 8);
        tcp[23] = (uint8_t)TCP_MSS;
    }
    for (uint16_t i = 0; i < plen; i++) {
        tcp[thlen + i] = payload[i];
    }
    uint16_t seg_len = (uint16_t)(thlen + plen);
    uint16_t csum = tcp_cksum(my_ip, tcb.rip, tcp, seg_len);
    tcp[16] = (uint8_t)(csum >> 8);
    tcp[17] = (uint8_t)csum;

    ip_fill(ip, tcb.rip, IP_PROTO_TCP, seg_len, (uint16_t)(0x8000 + (seq & 0x7FFF)));
    rtl8139_transmit(out, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + seg_len));

    /* Every ACK-bearing segment refreshes the advertised window. */
    if (flags & TCP_ACK) {
        tcb.last_adv_wnd = wnd;
    }
}

/* Send a bare ACK of the current rcv_nxt (also the window refresh). */
static void tcp_ack(void) {
    tcp_xmit(tcb.snd_nxt, TCP_ACK, NULL, 0, 0);
}

/* Zero the whole TCB and return it to CLOSED (net thread only). A pristine
 * CLOSED TCB is the ONLY claimable state — the UDP three-state lesson. */
static void tcp_reset_closed(void) {
    for (uint32_t i = 0; i < sizeof(tcb); i++) {
        ((uint8_t *)&tcb)[i] = 0;
    }
    tcb.state = TCPS_CLOSED;
}

/* Feed one received frame to the connection (net thread only, via net_rx).
 * ONE ordered pass: validate/checksum/RST, then process the ACK field,
 * then in-order payload, then the FIN flag — never mutually-exclusive
 * branches (a data+FIN segment must have both consumed and one ACK
 * covering both). */
void tcp_rx_demux(const uint8_t *frame, uint16_t len) {
    int st = tcb.state;
    if (st == TCPS_CLOSED || st == TCPS_ERROR) {
        return; /* ERROR is terminal for the net thread until abort retires it */
    }
    if (len < sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 20) {
        return;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_TCP) {
        return;
    }
    if (ntohs(ip->frag) & 0x3FFF) {
        return; /* fragment — no reassembly */
    }
    uint16_t total = ntohs(ip->total_len);
    if (total < sizeof(ip_hdr_t) + 20) {
        return;
    }
    if ((uint32_t)sizeof(eth_hdr_t) + total > len) {
        return; /* claims more than the frame carries */
    }
    /* seg length from IP total_len, NEVER the frame length — control
     * segments (54B) are padded to the 60B Ethernet minimum. */
    uint16_t seg_len = (uint16_t)(total - sizeof(ip_hdr_t));
    const uint8_t *tcp = frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t);

    /* 4-tuple match against the single connection. */
    if (!ip_eq(ip->src, tcb.rip) || !ip_eq(ip->dst, my_ip)) {
        return;
    }
    uint16_t sport = (uint16_t)((tcp[0] << 8) | tcp[1]);
    uint16_t dport = (uint16_t)((tcp[2] << 8) | tcp[3]);
    if (sport != tcb.rport || dport != tcb.lport) {
        return;
    }
    if (tcp_cksum(ip->src, ip->dst, tcp, seg_len) != 0) {
        return; /* mandatory checksum — SLIRP computes real ones */
    }
    uint8_t doff = (uint8_t)(tcp[12] >> 4);
    if (doff < 5 || (uint16_t)(doff * 4) > seg_len) {
        return; /* validate BEFORE computing payload_len (no underflow) */
    }
    uint8_t flags = tcp[13];
    uint32_t seq =
        ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) | ((uint32_t)tcp[6] << 8) | tcp[7];
    uint32_t ack =
        ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16) | ((uint32_t)tcp[10] << 8) | tcp[11];
    uint16_t payload_off = (uint16_t)(doff * 4);
    uint16_t payload_len = (uint16_t)(seg_len - payload_off);
    const uint8_t *payload = tcp + payload_off;

    if (flags & TCP_RST) {
        tcb.state = TCPS_ERROR;
        return;
    }

    /* SYN_SENT: the only acceptable segment is our SYN|ACK. */
    if (st == TCPS_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tcb.snd_nxt) {
            tcb.rcv_nxt = seq + 1;
            tcb.snd_una = ack;
            tcb.state = TCPS_ESTABLISHED;
            tcp_ack(); /* completes the handshake */
        }
        return;
    }

    /* (1) ACK field — advance snd_una (forward only, wrap-safe). */
    if (flags & TCP_ACK) {
        if (SEQ_GT(ack, tcb.snd_una) && !SEQ_GT(ack, tcb.snd_nxt)) {
            tcb.snd_una = ack;
        }
        if (tcb.snd_una == tcb.snd_nxt) {
            tcb.tx_pending = 0; /* data (or FIN) fully acknowledged */
            if (tcb.fin_sent) {
                if (tcb.state == TCPS_FIN_WAIT_1) {
                    tcb.state = TCPS_FIN_WAIT_2;
                } else if (tcb.state == TCPS_LAST_ACK) {
                    tcp_reset_closed(); /* clean close complete */
                    return;
                } else if (tcb.state == TCPS_CLOSING) {
                    tcb.state = TCPS_TIME_WAIT;
                    tcb.timer_tick = (uint32_t)timer_get_ticks();
                }
            }
        }
    }

    /* (2) in-order payload. */
    if (payload_len > 0 && seq == tcb.rcv_nxt) {
        if (payload_len <= tcp_rx_free()) {
            uint32_t head = tcb.rx_head;
            for (uint16_t i = 0; i < payload_len; i++) {
                tcb.rx[(head + i) & (TCP_RX_BUF - 1)] = payload[i];
            }
            publish_barrier();
            tcb.rx_head = head + payload_len;
            tcb.rcv_nxt += payload_len;
        }
        /* else ring full: drop, don't advance — the ACK below advertises
         * the (small) window and the peer retransmits. */
    }

    /* (3) FIN — only once every preceding byte is in-order-consumed, i.e.
     * the FIN's sequence number equals the (possibly just-advanced)
     * rcv_nxt. */
    if ((flags & TCP_FIN) && (seq + payload_len) == tcb.rcv_nxt) {
        tcb.rcv_nxt += 1;
        tcb.fin_received = 1;
        if (tcb.state == TCPS_ESTABLISHED) {
            tcb.state = TCPS_CLOSE_WAIT;
        } else if (tcb.state == TCPS_FIN_WAIT_1) {
            tcb.state = TCPS_CLOSING;
        } else if (tcb.state == TCPS_FIN_WAIT_2) {
            tcb.state = TCPS_TIME_WAIT;
            tcb.timer_tick = (uint32_t)timer_get_ticks();
        }
    }

    /* (4) ACK anything that occupied sequence space (data or FIN) OR an
     * old/duplicate segment — a pure ACK gets no reply (no ACK storm).
     * One ACK, reflecting the final rcv_nxt (covers data and FIN). */
    if (payload_len > 0 || (flags & TCP_FIN)) {
        tcp_ack();
    }
}

/* Allocate the next TCP ephemeral port (advances every active open so a
 * stale in-flight segment from a prior connection cannot 4-tuple match). */
static uint16_t tcp_ephemeral(void) {
    uint16_t p = tcp_ephemeral_next;
    tcp_ephemeral_next = (uint16_t)(p == 65535 ? TCP_EPHEMERAL_BASE : p + 1);
    return p;
}

/* Net-thread TCP servicing: retire aborts, start connects, drive
 * retransmission/close/TIME_WAIT. Runs each service-loop wake. */
void tcp_service(void) {
    uint32_t now = (uint32_t)timer_get_ticks();

    /* abort_req wins over everything and always retires to CLOSED. */
    if (tcb.abort_req) {
        if (tcb.state != TCPS_CLOSED && tcb.state != TCPS_SYN_SENT && tcb.have_mac) {
            tcp_xmit(tcb.snd_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
        }
        tcp_reset_closed();
        return;
    }
    if (tcb.state == TCPS_ERROR) {
        return; /* terminal until a syscall/cleanup posts abort_req */
    }
    if (tcb.state == TCPS_CLOSED) {
        if (tcb.connect_req) {
            tcb.connect_req = 0;
            for (int i = 0; i < 4; i++) {
                tcb.rip[i] = tcb.creq_ip[i];
            }
            tcb.rport = tcb.creq_port;
            if (!resolve_mac(net_next_hop(tcb.rip), tcb.nexthop_mac)) {
                tcb.state = TCPS_ERROR;
                return;
            }
            tcb.have_mac = 1;
            tcb.iss = (uint32_t)timer_get_ticks() * 2654435761u + 0x1000;
            tcb.snd_una = tcb.snd_nxt = tcb.iss + 1; /* the SYN occupies iss */
            tcb.rcv_nxt = 0;
            tcb.state = TCPS_SYN_SENT;
            tcb.retries = 0;
            tcb.retx_tick = now;
            tcp_xmit(tcb.iss, TCP_SYN, NULL, 0, 1);
        }
        return;
    }

    /* SYN retransmit — state-driven, independent of snd_una/snd_nxt. */
    if (tcb.state == TCPS_SYN_SENT) {
        if ((uint32_t)(now - tcb.retx_tick) >= TCP_RETX_TICKS) {
            if (++tcb.retries > TCP_MAX_RETRIES) {
                tcb.state = TCPS_ERROR;
                return;
            }
            tcb.retx_tick = now;
            tcp_xmit(tcb.iss, TCP_SYN, NULL, 0, 1);
        }
        return;
    }

    /* Data: transmit fresh, or retransmit an outstanding segment. */
    if (tcb.tx_pending && (tcb.state == TCPS_ESTABLISHED || tcb.state == TCPS_CLOSE_WAIT)) {
        if (tcb.snd_una == tcb.snd_nxt) { /* fresh (nothing outstanding) */
            tcp_xmit(tcb.snd_nxt, TCP_PSH | TCP_ACK, tcb.tx, tcb.tx_len, 0);
            tcb.snd_nxt += tcb.tx_len;
            tcb.retries = 0;
            tcb.retx_tick = now;
        } else if ((uint32_t)(now - tcb.retx_tick) >= TCP_RETX_TICKS) {
            if (++tcb.retries > TCP_MAX_RETRIES) {
                tcb.state = TCPS_ERROR;
                return;
            }
            tcp_xmit(tcb.snd_una, TCP_PSH | TCP_ACK, tcb.tx, tcb.tx_len, 0);
            tcb.retx_tick = now;
        }
    }

    /* Graceful close: once our data has drained, send the FIN. */
    if (tcb.close_req && !tcb.fin_sent && !tcb.tx_pending && tcb.snd_una == tcb.snd_nxt &&
        (tcb.state == TCPS_ESTABLISHED || tcb.state == TCPS_CLOSE_WAIT)) {
        tcp_xmit(tcb.snd_nxt, TCP_FIN | TCP_ACK, NULL, 0, 0);
        tcb.snd_nxt += 1;
        tcb.fin_sent = 1;
        tcb.retries = 0;
        tcb.retx_tick = now;
        tcb.state = (tcb.state == TCPS_ESTABLISHED) ? TCPS_FIN_WAIT_1 : TCPS_LAST_ACK;
    }

    /* FIN retransmit. */
    if (tcb.fin_sent && SEQ_GT(tcb.snd_nxt, tcb.snd_una) &&
        (tcb.state == TCPS_FIN_WAIT_1 || tcb.state == TCPS_LAST_ACK || tcb.state == TCPS_CLOSING)) {
        if ((uint32_t)(now - tcb.retx_tick) >= TCP_RETX_TICKS) {
            if (++tcb.retries > TCP_MAX_RETRIES) {
                tcb.state = TCPS_ERROR;
                return;
            }
            tcb.retx_tick = now;
            tcp_xmit(tcb.snd_nxt - 1, TCP_FIN | TCP_ACK, NULL, 0, 0);
        }
    }

    /* Proactive window update: after the app drains the ring, tell the
     * peer the window reopened (it will not probe promptly on its own). */
    if ((tcb.state == TCPS_ESTABLISHED || tcb.state == TCPS_CLOSE_WAIT) &&
        tcp_rx_free() >= TCP_MSS && tcb.last_adv_wnd < TCP_MSS) {
        tcp_ack();
    }

    /* TIME_WAIT expiry (shortened). */
    if (tcb.state == TCPS_TIME_WAIT && (uint32_t)(now - tcb.timer_tick) >= TCP_TIMEWAIT_TICKS) {
        tcp_reset_closed();
    }
}

/* ---- syscall-facing TCP ops (cli'd context; never touch the NIC) ---- */

long net_tcp_connect(uint32_t pid, const uint8_t *ip, uint16_t port) {
    if (!net_nic_present()) {
        return NET_TCP_ENONET;
    }
    int st = tcb.state;
    if (st == TCPS_ESTABLISHED && tcb.owner_pid == pid) {
        return 0;
    }
    if (st == TCPS_ERROR && tcb.owner_pid == pid) {
        tcb.abort_req = 1; /* let the net thread retire it, then retry */
        return NET_TCP_EIO;
    }
    if (st == TCPS_CLOSED) {
        if (tcb.connect_req) {
            return NET_TCP_WOULDBLOCK; /* already posted, awaiting the net thread */
        }
        for (int i = 0; i < 4; i++) {
            tcb.creq_ip[i] = ip[i];
        }
        tcb.creq_port = port;
        tcb.lport = tcp_ephemeral();
        tcb.owner_pid = pid;
        publish_barrier();
        tcb.connect_req = 1; /* the publish store */
        return NET_TCP_WOULDBLOCK;
    }
    if (tcb.owner_pid == pid) {
        return NET_TCP_WOULDBLOCK; /* SYN_SENT / TIME_WAIT / closing — poll */
    }
    return NET_TCP_EINVAL; /* someone else owns the one connection */
}

long net_tcp_send(uint32_t pid, const uint8_t *buf, uint16_t len) {
    if (tcb.owner_pid != pid) {
        return NET_TCP_EINVAL;
    }
    if (tcb.state == TCPS_ERROR) {
        return NET_TCP_EIO;
    }
    if (tcb.state != TCPS_ESTABLISHED) {
        return NET_TCP_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (len > TCP_MSS) {
        len = TCP_MSS;
    }
    if (tcb.tx_pending) {
        return NET_TCP_WOULDBLOCK; /* a segment is still outstanding */
    }
    for (uint16_t i = 0; i < len; i++) {
        tcb.tx[i] = buf[i];
    }
    tcb.tx_len = len;
    publish_barrier();
    tcb.tx_pending = 1;
    return len;
}

long net_tcp_recv(uint32_t pid, uint8_t *buf, uint16_t maxlen) {
    if (tcb.owner_pid != pid) {
        return NET_TCP_EINVAL;
    }
    uint32_t avail = tcb.rx_head - tcb.rx_tail;
    if (avail == 0) {
        if (tcb.fin_received) {
            return 0; /* EOF — peer closed and the ring is drained */
        }
        if (tcb.state == TCPS_ERROR) {
            return NET_TCP_EIO;
        }
        if (tcb.state == TCPS_CLOSED) {
            return NET_TCP_EIO;
        }
        return NET_TCP_WOULDBLOCK;
    }
    uint32_t n = avail;
    if (n > maxlen) {
        n = maxlen;
    }
    uint32_t tail = tcb.rx_tail;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = tcb.rx[(tail + i) & (TCP_RX_BUF - 1)];
    }
    publish_barrier();
    tcb.rx_tail = tail + n;
    return (long)n;
}

long net_tcp_close(uint32_t pid) {
    if (tcb.owner_pid != pid) {
        return (tcb.state == TCPS_CLOSED) ? 0 : NET_TCP_EINVAL;
    }
    if (tcb.state == TCPS_CLOSED) {
        return 0;
    }
    if (tcb.state == TCPS_ERROR) {
        tcb.abort_req = 1; /* retire the broken connection */
        return 0;
    }
    tcb.close_req = 1;
    return NET_TCP_WOULDBLOCK; /* poll until CLOSED */
}

long net_tcp_status(uint32_t pid) {
    if (tcb.owner_pid != pid) {
        return NET_TCP_EINVAL;
    }
    int st = tcb.state;
    if (st == TCPS_ESTABLISHED) {
        return 0;
    }
    if (st == TCPS_ERROR) {
        return NET_TCP_EIO;
    }
    return NET_TCP_WOULDBLOCK;
}

void net_tcp_cleanup(uint32_t pid) {
    if (tcb.state != TCPS_CLOSED && tcb.owner_pid == pid) {
        tcb.abort_req = 1; /* the net thread RSTs (if past SYN) and retires */
    }
}
