/**
 * QuantumOS network layer implementation (epic #73 phase 1).
 *
 * Ethernet + ARP over the RTL8139. All multi-byte network fields are
 * big-endian ("network order"); x86 is little-endian, so htons/ntohs
 * appear on every field wider than a byte.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net.h>
#include <kernel/rtl8139.h>
#include <kernel/boot.h>

/* QEMU user-net (SLIRP) addresses: the guest is 10.0.2.15, the gateway
 * (and the host we ARP for) is 10.0.2.2. */
static const uint8_t IP_SELF[4] = {10, 0, 2, 15};
static const uint8_t IP_GATEWAY[4] = {10, 0, 2, 2};

#define ETH_TYPE_ARP 0x0806
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

static uint8_t self_mac[ETH_ADDR_LEN];

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
#define ntohs(v) htons(v)

/* Ethernet header (14 bytes). */
typedef struct __attribute__((packed)) {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t type; /* big-endian */
} eth_hdr_t;

/* ARP packet for IPv4-over-Ethernet (28 bytes). */
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

static int ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

void net_init(void) {
    if (!rtl8139_present()) {
        return;
    }
    rtl8139_get_mac(self_mac);
}

/* Build and send a broadcast ARP request for `target_ip`. */
static int arp_send_request(const uint8_t *target_ip) {
    uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = 0xFF; /* broadcast */
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
        arp->tha[i] = 0x00; /* unknown — what we're asking for */
    }
    for (int i = 0; i < 4; i++) {
        arp->spa[i] = IP_SELF[i];
        arp->tpa[i] = target_ip[i];
    }

    return rtl8139_transmit(frame, sizeof(frame));
}

/* Inspect a received frame; if it is an ARP reply from `want_ip`, copy
 * the sender's MAC into out_mac and return 1. */
static int arp_match_reply(const uint8_t *frame, uint16_t len, const uint8_t *want_ip,
                           uint8_t *out_mac) {
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_ARP) {
        return 0;
    }
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    if (ntohs(arp->oper) != ARP_OP_REPLY) {
        return 0;
    }
    if (!ip_eq(arp->spa, want_ip)) {
        return 0;
    }
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out_mac[i] = arp->sha[i];
    }
    return 1;
}

void net_selftest(void) {
    if (!rtl8139_present()) {
        boot_log("NET: no NIC — ARP self-test skipped");
        return;
    }

    if (arp_send_request(IP_GATEWAY) != 0) {
        boot_log("NET: ARP request transmit failed");
        return;
    }

    /* Wait (bounded) for the reply. The RX IRQ fills the driver queue;
     * between polls we `hlt` so the CPU sleeps until the next interrupt
     * (the timer, or — the point — the NIC's RX IRQ) and the timer keeps
     * advancing. This body runs in a kernel thread with IF=1, so hlt is
     * safe: a tight IF-blind spin here would deadlock, since the RX IRQ
     * could never preempt it to fill the queue. The request is re-sent
     * periodically so a single dropped frame is not a false timeout. */
    uint8_t frame[RTL_FRAME_MAX];
    uint8_t gw_mac[ETH_ADDR_LEN];
    for (int tries = 0; tries < 500; tries++) {
        uint16_t n = rtl8139_receive(frame, sizeof(frame));
        if (n > 0 && arp_match_reply(frame, n, IP_GATEWAY, gw_mac)) {
            boot_log("NET: ARP 10.0.2.2 is at MAC:");
            for (int i = 0; i < ETH_ADDR_LEN; i++) {
                early_console_write_hex(gw_mac[i]);
            }
            return;
        }
        if (n == 0) {
            if (tries > 0 && tries % 64 == 0) {
                arp_send_request(IP_GATEWAY); /* retransmit; ignore errors */
            }
            __asm__ volatile("hlt"); /* sleep until the next IRQ */
        }
    }
    boot_log("NET: ARP 10.0.2.2 timed out (no reply)");
}
