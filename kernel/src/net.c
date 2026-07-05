/**
 * QuantumOS network layer implementation (epic #73 phases 1-2).
 *
 * Ethernet + ARP (phase 1) and IPv4 + UDP + a DHCP client (phase 2) over
 * the RTL8139. All multi-byte network fields are big-endian ("network
 * order"); x86 is little-endian, so htons/htonl appear on every field
 * wider than a byte.
 *
 * The boot self-test proves the stack end to end against QEMU user-net
 * (SLIRP): it ARP-resolves the gateway, then runs a full DHCP exchange
 * to obtain the 10.0.2.15 lease — which exercises Ethernet TX/RX, IPv4,
 * UDP, checksums, and broadcast, both directions.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net.h>
#include <kernel/rtl8139.h>
#include <kernel/boot.h>

/* QEMU user-net (SLIRP): gateway 10.0.2.2, DHCP hands out 10.0.2.15. */
static const uint8_t IP_GATEWAY[4] = {10, 0, 2, 2};
static const uint8_t IP_ZERO[4] = {0, 0, 0, 0};
static const uint8_t IP_BCAST[4] = {255, 255, 255, 255};
static const uint8_t MAC_BCAST[ETH_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP 0x0800
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define IP_PROTO_UDP 17
#define DHCP_SPORT 68
#define DHCP_DPORT 67

static uint8_t self_mac[ETH_ADDR_LEN];

/* DHCP outcome (filled by the exchange). */
static uint8_t my_ip[4];
static int dhcp_have_lease;

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
#define ntohs(v) htons(v)
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

/* ---- packet headers (all packed) ---- */
typedef struct __attribute__((packed)) {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[ETH_ADDR_LEN];
    uint8_t spa[4];
    uint8_t tha[ETH_ADDR_LEN];
    uint8_t tpa[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl; /* 0x45: IPv4, 5*4=20 byte header */
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint8_t src[4];
    uint8_t dst[4];
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t checksum;
} udp_hdr_t;

/* BOOTP/DHCP fixed header (before options). */
typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic; /* 0x63825363 */
} dhcp_hdr_t;

#define DHCP_XID 0x51050403u
#define DHCP_MAGIC 0x63825363u
#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY 2
#define DHCP_OPT_MSGTYPE 53
#define DHCP_OPT_REQIP 50
#define DHCP_OPT_SERVERID 54
#define DHCP_OPT_END 255
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

/* DHCP client state. */
static int dhcp_state; /* 0 idle, 1 sent DISCOVER, 2 sent REQUEST */
static uint8_t dhcp_offer_ip[4];
static uint8_t dhcp_server_id[4];

static int ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* 16-bit one's-complement checksum over `len` bytes. */
static uint16_t checksum16(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)(data[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void net_init(void) {
    if (!rtl8139_present()) {
        return;
    }
    rtl8139_get_mac(self_mac);
    dhcp_have_lease = 0;
    dhcp_state = 0;
}

/* ---- ARP ---- */
static int arp_send_request(const uint8_t *target_ip, const uint8_t *sender_ip) {
    uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = 0xFF;
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_ARP);
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ARP_PTYPE_IP);
    arp->hlen = ETH_ADDR_LEN;
    arp->plen = 4;
    arp->oper = htons(ARP_OP_REQUEST);
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        arp->sha[i] = self_mac[i];
        arp->tha[i] = 0x00;
    }
    for (int i = 0; i < 4; i++) {
        arp->spa[i] = sender_ip[i];
        arp->tpa[i] = target_ip[i];
    }
    return rtl8139_transmit(frame, sizeof(frame));
}

static int arp_is_reply_from(const uint8_t *frame, uint16_t len, const uint8_t *want_ip,
                             uint8_t *out_mac) {
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_ARP) {
        return 0;
    }
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    if (ntohs(arp->oper) != ARP_OP_REPLY || !ip_eq(arp->spa, want_ip)) {
        return 0;
    }
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out_mac[i] = arp->sha[i];
    }
    return 1;
}

/* ---- DHCP ---- */

/* Build eth+ip+udp+dhcp for a broadcast DHCP message of the given type.
 * `req_ip`/`server_id` are used only for REQUEST (option 50/54). Returns
 * the total frame length. */
static uint16_t dhcp_build(uint8_t *frame, uint8_t msgtype, const uint8_t *req_ip,
                           const uint8_t *server_id) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    dhcp_hdr_t *dh = (dhcp_hdr_t *)((uint8_t *)udp + sizeof(udp_hdr_t));
    uint8_t *opt = (uint8_t *)dh + sizeof(dhcp_hdr_t);
    uint8_t *opt_start = opt;

    /* Ethernet: broadcast. */
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = MAC_BCAST[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* DHCP fixed header. */
    for (uint32_t i = 0; i < sizeof(dhcp_hdr_t); i++) {
        ((uint8_t *)dh)[i] = 0;
    }
    dh->op = DHCP_OP_REQUEST;
    dh->htype = 1;
    dh->hlen = ETH_ADDR_LEN;
    dh->xid = htonl(DHCP_XID);
    dh->flags = htons(0x8000); /* broadcast reply (we have no IP yet) */
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        dh->chaddr[i] = self_mac[i];
    }
    dh->magic = htonl(DHCP_MAGIC);

    /* Options. */
    *opt++ = DHCP_OPT_MSGTYPE;
    *opt++ = 1;
    *opt++ = msgtype;
    if (msgtype == DHCP_REQUEST) {
        *opt++ = DHCP_OPT_REQIP;
        *opt++ = 4;
        for (int i = 0; i < 4; i++) {
            *opt++ = req_ip[i];
        }
        *opt++ = DHCP_OPT_SERVERID;
        *opt++ = 4;
        for (int i = 0; i < 4; i++) {
            *opt++ = server_id[i];
        }
    }
    *opt++ = DHCP_OPT_END;

    uint16_t dhcp_len = (uint16_t)(sizeof(dhcp_hdr_t) + (opt - opt_start));
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + dhcp_len);
    uint16_t ip_len = (uint16_t)(sizeof(ip_hdr_t) + udp_len);

    /* UDP (checksum 0 = not computed, legal for IPv4). */
    udp->sport = htons(DHCP_SPORT);
    udp->dport = htons(DHCP_DPORT);
    udp->len = htons(udp_len);
    udp->checksum = 0;

    /* IPv4: broadcast src 0.0.0.0 -> 255.255.255.255. */
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(ip_len);
    ip->id = htons(0x1234);
    ip->frag = htons(0x0000);
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->checksum = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = IP_ZERO[i];
        ip->dst[i] = IP_BCAST[i];
    }
    ip->checksum = htons(checksum16((const uint8_t *)ip, sizeof(ip_hdr_t)));

    return (uint16_t)(sizeof(eth_hdr_t) + ip_len);
}

/* Find DHCP option `code` in the options area; returns its value pointer
 * and length via out-params, or 0 if absent. */
static int dhcp_find_opt(const uint8_t *opts, uint32_t optlen, uint8_t code, const uint8_t **val,
                         uint8_t *vlen) {
    uint32_t i = 0;
    while (i + 1 < optlen) {
        uint8_t c = opts[i];
        if (c == DHCP_OPT_END) {
            break;
        }
        if (c == 0) { /* pad */
            i++;
            continue;
        }
        uint8_t l = opts[i + 1];
        if (i + 2 + l > optlen) {
            break;
        }
        if (c == code) {
            *val = opts + i + 2;
            *vlen = l;
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}

/* Parse a received DHCP reply of the wanted type; on match, copy yiaddr
 * and the server-id option out. Returns the message type, or 0. */
static uint8_t dhcp_parse(const uint8_t *frame, uint16_t len, uint8_t *yiaddr, uint8_t *server_id) {
    uint32_t need = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t);
    if (len < need) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return 0;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_UDP) {
        return 0;
    }
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + sizeof(ip_hdr_t));
    if (ntohs(udp->sport) != DHCP_DPORT || ntohs(udp->dport) != DHCP_SPORT) {
        return 0;
    }
    const dhcp_hdr_t *dh = (const dhcp_hdr_t *)((const uint8_t *)udp + sizeof(udp_hdr_t));
    /* xid and magic are stored in network order on both sides, so compare
     * the raw (already byte-swapped) values directly. */
    if (dh->op != DHCP_OP_REPLY || dh->xid != htonl(DHCP_XID) || dh->magic != htonl(DHCP_MAGIC)) {
        return 0;
    }

    const uint8_t *opts = (const uint8_t *)dh + sizeof(dhcp_hdr_t);
    uint32_t optlen = len - need;
    const uint8_t *v;
    uint8_t vl;
    if (!dhcp_find_opt(opts, optlen, DHCP_OPT_MSGTYPE, &v, &vl) || vl < 1) {
        return 0;
    }
    uint8_t msgtype = v[0];
    for (int i = 0; i < 4; i++) {
        yiaddr[i] = dh->yiaddr[i];
    }
    if (dhcp_find_opt(opts, optlen, DHCP_OPT_SERVERID, &v, &vl) && vl == 4) {
        for (int i = 0; i < 4; i++) {
            server_id[i] = v[i];
        }
    }
    return msgtype;
}

/* Feed one received frame to the DHCP state machine. */
static void dhcp_rx(const uint8_t *frame, uint16_t len) {
    uint8_t yiaddr[4], server_id[4] = {0, 0, 0, 0};
    uint8_t t = dhcp_parse(frame, len, yiaddr, server_id);
    if (t == DHCP_OFFER && dhcp_state == 1) {
        for (int i = 0; i < 4; i++) {
            dhcp_offer_ip[i] = yiaddr[i];
            dhcp_server_id[i] = server_id[i];
        }
        uint8_t
            out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t) + 32];
        uint16_t n = dhcp_build(out, DHCP_REQUEST, dhcp_offer_ip, dhcp_server_id);
        rtl8139_transmit(out, n);
        dhcp_state = 2;
    } else if (t == DHCP_ACK && dhcp_state == 2) {
        for (int i = 0; i < 4; i++) {
            my_ip[i] = yiaddr[i];
        }
        dhcp_have_lease = 1;
    }
}

/* ---- self-test: ARP then DHCP ---- */

static void log_ip(const char *label, const uint8_t *ip) {
    char b[48];
    int o = 0;
    while (*label) {
        b[o++] = *label++;
    }
    for (int i = 0; i < 4; i++) {
        unsigned v = ip[i];
        char t[3];
        int n = 0;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v) {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (n) {
            b[o++] = t[--n];
        }
        if (i < 3) {
            b[o++] = '.';
        }
    }
    b[o] = 0;
    boot_log(b);
}

void net_selftest(void) {
    if (!rtl8139_present()) {
        boot_log("NET: no NIC — self-test skipped");
        return;
    }

    /* Phase 1: ARP-resolve the gateway (link-layer proof). We have no
     * DHCP lease yet, but SLIRP answers ARP regardless of the requester's
     * address, so use the address SLIRP assigns (10.0.2.15) as sender. */
    static const uint8_t IP_PRE_DHCP[4] = {10, 0, 2, 15};
    arp_send_request(IP_GATEWAY, IP_PRE_DHCP);

    uint8_t frame[RTL_FRAME_MAX];
    uint8_t gw_mac[ETH_ADDR_LEN];
    int arp_ok = 0;
    for (int tries = 0; tries < 200 && !arp_ok; tries++) {
        uint16_t n = rtl8139_receive(frame, sizeof(frame));
        if (n > 0 && arp_is_reply_from(frame, n, IP_GATEWAY, gw_mac)) {
            boot_log("NET: ARP 10.0.2.2 is at MAC:");
            for (int i = 0; i < ETH_ADDR_LEN; i++) {
                early_console_write_hex(gw_mac[i]);
            }
            arp_ok = 1;
        } else if (n == 0) {
            if (tries > 0 && tries % 64 == 0) {
                arp_send_request(IP_GATEWAY, IP_PRE_DHCP);
            }
            __asm__ volatile("hlt");
        }
    }
    if (!arp_ok) {
        boot_log("NET: ARP 10.0.2.2 timed out (no reply)");
    }

    /* Phase 2: full DHCP exchange (DISCOVER -> OFFER -> REQUEST -> ACK).
     * Getting the lease exercises Ethernet, IPv4, UDP, checksums, and
     * broadcast, both directions. */
    {
        uint8_t
            out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t) + 32];
        uint16_t n = dhcp_build(out, DHCP_DISCOVER, IP_ZERO, IP_ZERO);
        if (rtl8139_transmit(out, n) == 0) {
            dhcp_state = 1;
        }
    }
    for (int tries = 0; tries < 600 && !dhcp_have_lease; tries++) {
        uint16_t n = rtl8139_receive(frame, sizeof(frame));
        if (n > 0) {
            dhcp_rx(frame, n);
        } else {
            if (tries > 0 && tries % 128 == 0 && dhcp_state == 1) {
                /* Re-DISCOVER if the offer was lost. */
                uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) +
                            sizeof(dhcp_hdr_t) + 32];
                uint16_t m = dhcp_build(out, DHCP_DISCOVER, IP_ZERO, IP_ZERO);
                rtl8139_transmit(out, m);
            }
            __asm__ volatile("hlt");
        }
    }
    if (dhcp_have_lease) {
        log_ip("NET: DHCP lease ", my_ip);
    } else {
        boot_log("NET: DHCP timed out (no lease)");
    }
}
