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

/* ---- IPv4 unicast + ICMP + DNS (phase 3) ---- */

#define IP_PROTO_ICMP 1
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0
#define DNS_PORT 53
static const uint8_t IP_DNS[4] = {10, 0, 2, 3}; /* SLIRP's DNS proxy */

/* Fill an IPv4 header for a unicast packet of `payload_len` bytes to
 * `dst`, from `my_ip`. Computes the header checksum. */
static void ip_fill(ip_hdr_t *ip, const uint8_t *dst, uint8_t proto, uint16_t payload_len,
                    uint16_t ident) {
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + payload_len));
    ip->id = htons(ident);
    ip->frag = htons(0x0000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = my_ip[i];
        ip->dst[i] = dst[i];
    }
    ip->checksum = htons(checksum16((const uint8_t *)ip, sizeof(ip_hdr_t)));
}

/* Resolve `ip`'s MAC via ARP (bounded, pumping the RX queue). Returns 1
 * and fills out_mac on success. */
static int arp_resolve(const uint8_t *ip, const uint8_t *sender_ip, uint8_t *out_mac) {
    arp_send_request(ip, sender_ip);
    uint8_t frame[RTL_FRAME_MAX];
    for (int tries = 0; tries < 200; tries++) {
        uint16_t n = rtl8139_receive(frame, sizeof(frame));
        if (n > 0 && arp_is_reply_from(frame, n, ip, out_mac)) {
            return 1;
        }
        if (n == 0) {
            if (tries > 0 && tries % 64 == 0) {
                arp_send_request(ip, sender_ip);
            }
            __asm__ volatile("hlt");
        }
    }
    return 0;
}

/* ICMP echo request to `dst` (dst_mac is its resolved link address).
 * `ident`/`seq` tag the request; returns the built frame length. */
static uint16_t icmp_build(uint8_t *frame, const uint8_t *dst, const uint8_t *dst_mac,
                           uint16_t ident, uint16_t seq) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    uint8_t *icmp = (uint8_t *)ip + sizeof(ip_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = dst_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* ICMP echo: type(1) code(1) csum(2) id(2) seq(2) + 32 payload bytes. */
    uint16_t icmp_len = 8 + 32;
    for (uint16_t i = 0; i < icmp_len; i++) {
        icmp[i] = 0;
    }
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0;
    icmp[4] = (uint8_t)(ident >> 8);
    icmp[5] = (uint8_t)ident;
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)seq;
    for (uint16_t i = 0; i < 32; i++) {
        icmp[8 + i] = (uint8_t)('a' + (i % 26));
    }
    uint16_t csum = checksum16(icmp, icmp_len);
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)csum;

    ip_fill(ip, dst, IP_PROTO_ICMP, icmp_len, ident);
    return (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + icmp_len);
}

/* Is `frame` an ICMP echo reply from `src` with our id/seq? */
static int icmp_is_reply(const uint8_t *frame, uint16_t len, const uint8_t *src, uint16_t ident,
                         uint16_t seq) {
    uint32_t need = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 8;
    if (len < need) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return 0;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_ICMP || !ip_eq(ip->src, src)) {
        return 0;
    }
    const uint8_t *icmp = (const uint8_t *)ip + sizeof(ip_hdr_t);
    uint16_t rid = (uint16_t)((icmp[4] << 8) | icmp[5]);
    uint16_t rseq = (uint16_t)((icmp[6] << 8) | icmp[7]);
    return icmp[0] == ICMP_ECHO_REPLY && rid == ident && rseq == seq;
}

/* Build a DNS A-query for `name` to the resolver. UDP over IPv4. Returns
 * the frame length, or 0 if the name doesn't fit. */
static uint16_t dns_build(uint8_t *frame, const uint8_t *dns_mac, const char *name, uint16_t txid) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    uint8_t *dns = (uint8_t *)udp + sizeof(udp_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = dns_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* DNS header: id, flags 0x0100 (recursion desired), qdcount 1. */
    dns[0] = (uint8_t)(txid >> 8);
    dns[1] = (uint8_t)txid;
    dns[2] = 0x01;
    dns[3] = 0x00;
    dns[4] = 0x00;
    dns[5] = 0x01; /* qdcount = 1 */
    dns[6] = dns[7] = dns[8] = dns[9] = dns[10] = dns[11] = 0;
    int p = 12;

    /* QNAME: length-prefixed labels. */
    int label_start = p++;
    int label_len = 0;
    for (const char *c = name; *c; c++) {
        if (*c == '.') {
            dns[label_start] = (uint8_t)label_len;
            label_start = p++;
            label_len = 0;
        } else {
            if (p >= 200) {
                return 0;
            }
            dns[p++] = (uint8_t)*c;
            label_len++;
        }
    }
    dns[label_start] = (uint8_t)label_len;
    dns[p++] = 0x00; /* root label */
    dns[p++] = 0x00; /* QTYPE A = 1 */
    dns[p++] = 0x01;
    dns[p++] = 0x00; /* QCLASS IN = 1 */
    dns[p++] = 0x01;

    uint16_t dns_len = (uint16_t)p;
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + dns_len);
    udp->sport = htons(40000);
    udp->dport = htons(DNS_PORT);
    udp->len = htons(udp_len);
    udp->checksum = 0;

    ip_fill(ip, IP_DNS, IP_PROTO_UDP, udp_len, txid);
    return (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + udp_len);
}

/* Parse a DNS response; on a matching txid with at least one A record,
 * copy the first A address into out_ip and return 1. */
static int dns_parse(const uint8_t *frame, uint16_t len, uint16_t txid, uint8_t *out_ip) {
    uint32_t base = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t);
    if (len < base + 12) {
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
    if (ntohs(udp->sport) != DNS_PORT) {
        return 0;
    }
    const uint8_t *dns = frame + base;
    uint16_t rid = (uint16_t)((dns[0] << 8) | dns[1]);
    if (rid != txid) {
        return 0;
    }
    uint16_t qd = (uint16_t)((dns[4] << 8) | dns[5]);
    uint16_t an = (uint16_t)((dns[6] << 8) | dns[7]);
    if (an == 0) {
        return 0;
    }

    /* Skip the header (12) and the question section. */
    uint32_t off = base + 12;
    for (uint16_t q = 0; q < qd; q++) {
        while (off < len && frame[off] != 0) {
            if ((frame[off] & 0xC0) == 0xC0) { /* compression pointer */
                off += 2;
                goto qdone;
            }
            off += frame[off] + 1;
        }
        off += 1; /* the 0 root label */
    qdone:
        off += 4; /* QTYPE + QCLASS */
    }

    /* Walk answers; return the first A record (type 1, class IN, rdlen 4). */
    for (uint16_t a = 0; a < an; a++) {
        if (off + 2 > len) {
            return 0;
        }
        if ((frame[off] & 0xC0) == 0xC0) {
            off += 2; /* compressed name */
        } else {
            while (off < len && frame[off] != 0) {
                off += frame[off] + 1;
            }
            off += 1;
        }
        if (off + 10 > len) {
            return 0;
        }
        uint16_t type = (uint16_t)((frame[off] << 8) | frame[off + 1]);
        uint16_t rdlen = (uint16_t)((frame[off + 8] << 8) | frame[off + 9]);
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            for (int i = 0; i < 4; i++) {
                out_ip[i] = frame[off + i];
            }
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

/* Resolve `host` to an IPv4 A record via SLIRP's DNS proxy. Bounded,
 * pumps the RX queue (must run in the IF=1 net thread, not a cli'd
 * syscall). Returns 0 and fills out_ip on success, -1 on failure.
 * Requires a NIC and a DHCP lease (for the unicast source address). */
static int dns_resolve(const char *host, uint16_t txid, uint8_t *out_ip) {
    if (!rtl8139_present() || !dhcp_have_lease) {
        return -1;
    }
    uint8_t dns_mac[ETH_ADDR_LEN];
    if (!arp_resolve(IP_DNS, my_ip, dns_mac)) {
        return -1;
    }
    uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + 256];
    uint16_t n = dns_build(out, dns_mac, host, txid);
    if (n == 0) {
        return -1;
    }
    rtl8139_transmit(out, n);

    uint8_t frame[RTL_FRAME_MAX];
    for (int tries = 0; tries < 400; tries++) {
        uint16_t m = rtl8139_receive(frame, sizeof(frame));
        if (m > 0 && dns_parse(frame, m, txid, out_ip)) {
            return 0;
        }
        if (m == 0) {
            if (tries > 0 && tries % 100 == 0) {
                rtl8139_transmit(out, n);
            }
            __asm__ volatile("hlt");
        }
    }
    return -1;
}

/* ---- ring-3 resolve service (epic #73 follow-up, #77) ----
 *
 * A syscall runs with interrupts disabled, so it cannot pump the RX ring
 * (the NIC IRQ can never fire) — DNS must happen in the IF=1 net thread.
 * SYS_RESOLVE posts a hostname here and the net thread services it; the
 * user polls. Single writer per field on one CPU, `resolve_state` (a
 * volatile, written last on post) is the handshake flag. */
#define RESOLVE_IDLE 0
#define RESOLVE_PENDING 1
#define RESOLVE_DONE 2
#define RESOLVE_FAILED 3
#define RESOLVE_HOST_MAX 64

static volatile int resolve_state;
static char resolve_host[RESOLVE_HOST_MAX];
static uint8_t resolve_ip[4];
static uint16_t resolve_txid = 0x2000;

int net_ready(void) {
    return rtl8139_present() && dhcp_have_lease;
}

int net_nic_present(void) {
    return rtl8139_present();
}

/* Post a resolve request (from SYS_RESOLVE). Returns 0 accepted, -1 if
 * the network isn't ready or a request is already in flight. */
int net_request_resolve(const char *host) {
    if (!net_ready()) {
        return -1;
    }
    if (resolve_state == RESOLVE_PENDING) {
        return -1; /* one at a time */
    }
    int i = 0;
    while (i < RESOLVE_HOST_MAX - 1 && host[i]) {
        resolve_host[i] = host[i];
        i++;
    }
    resolve_host[i] = '\0';
    resolve_state = RESOLVE_PENDING; /* written last — the handshake */
    return 0;
}

/* Poll the outcome (from SYS_RESOLVE). Returns 1 done (out_ip filled),
 * 0 still pending, -1 failed (also resets to idle). */
int net_poll_resolve(uint8_t *out_ip) {
    if (resolve_state == RESOLVE_DONE) {
        for (int i = 0; i < 4; i++) {
            out_ip[i] = resolve_ip[i];
        }
        resolve_state = RESOLVE_IDLE;
        return 1;
    }
    if (resolve_state == RESOLVE_FAILED) {
        resolve_state = RESOLVE_IDLE;
        return -1;
    }
    return 0;
}

/* Net thread body after the self-test: service resolve requests forever.
 * Wakes on every timer tick (hlt) and checks for a pending request. */
void net_service_loop(void) {
    for (;;) {
        if (resolve_state == RESOLVE_PENDING) {
            uint8_t ip[4];
            if (dns_resolve(resolve_host, resolve_txid++, ip) == 0) {
                for (int i = 0; i < 4; i++) {
                    resolve_ip[i] = ip[i];
                }
                resolve_state = RESOLVE_DONE;
            } else {
                resolve_state = RESOLVE_FAILED;
            }
        }
        __asm__ volatile("hlt");
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

    /* Phase 3 needs a lease (unicast IP source) and the gateway's MAC. */
    if (!arp_ok || !dhcp_have_lease) {
        return;
    }

    /* Phase 3a: ICMP echo to the gateway. SLIRP answers ping to 10.0.2.2
     * internally, so this is a fully self-contained round trip that
     * exercises unicast IPv4 + ICMP + the header checksum both ways. */
    {
        uint16_t ident = 0xBEEF, seq = 1;
        uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 40];
        uint16_t n = icmp_build(out, IP_GATEWAY, gw_mac, ident, seq);
        rtl8139_transmit(out, n);

        int pong = 0;
        for (int tries = 0; tries < 300 && !pong; tries++) {
            uint16_t m = rtl8139_receive(frame, sizeof(frame));
            if (m > 0 && icmp_is_reply(frame, m, IP_GATEWAY, ident, seq)) {
                pong = 1;
            } else if (m == 0) {
                if (tries > 0 && tries % 64 == 0) {
                    rtl8139_transmit(out, n);
                }
                __asm__ volatile("hlt");
            }
        }
        boot_log(pong ? "NET: ping 10.0.2.2 reply received" : "NET: ping 10.0.2.2 timed out");
    }

    /* Phase 3b: resolve a hostname through SLIRP's DNS proxy (10.0.2.3),
     * which forwards to the host resolver. Getting a well-formed A record
     * back is the capstone: ARP -> IPv4 -> UDP -> DNS, both directions. */
    {
        uint8_t resolved[4];
        if (dns_resolve("example.com", 0x1D15, resolved) == 0) {
            log_ip("NET: DNS example.com -> ", resolved);
        } else {
            boot_log("NET: DNS example.com timed out (no answer)");
        }
    }
}
