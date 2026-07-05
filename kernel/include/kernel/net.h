/**
 * QuantumOS network layer (epic #73 phase 1: Ethernet + ARP)
 *
 * A tiny link/network layer over the RTL8139. Phase 1 proves the driver
 * end to end by ARP-resolving the QEMU user-net gateway (10.0.2.2): an
 * ARP request goes out through TX, SLIRP's reply comes back through the
 * RX interrupt, and the resolved MAC is logged. Later phases add IPv4,
 * UDP, DHCP, DNS on the same send/receive spine.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NET_H
#define NET_H

#include <kernel/types.h>

/* Capability resource id for ring-3 network access (SYS_RESOLVE). The
 * NIC's I/O base is assigned dynamically by PCI, so — unlike the console
 * or disk — the resource id can't be a port base; this synthetic id
 * (the rtl8139's device id) just needs to be unique among device caps. */
#define DEVICE_ID_NET 0x8139

/* Bring up the network stack over the NIC (no-op if no NIC). */
void net_init(void);

/* Boot self-test: ARP-resolve the gateway, obtain a DHCP lease, ping the
 * gateway, and resolve a hostname — proving the stack end to end. Runs in
 * the net kernel thread (interrupts live), not inline in early boot. */
void net_selftest(void);

/* After the self-test, the net thread services ring-3 resolve requests
 * forever (SYS_RESOLVE posts them; a syscall can't do network I/O itself
 * because it runs with interrupts disabled). Never returns. */
void net_service_loop(void);

/* NIC up and a DHCP lease obtained (the network can send unicast IP). */
int net_ready(void);

/* Is a NIC present at all? Distinguishes "no network hardware" (a hard
 * failure) from "lease still coming up" (transient — keep polling). */
int net_nic_present(void);

/* Post a hostname to resolve (from SYS_RESOLVE). 0 accepted, -1 if the
 * network isn't ready or a request is already in flight. */
int net_request_resolve(const char *host);

/* Poll the resolve outcome (from SYS_RESOLVE). 1 done (out_ip filled),
 * 0 pending, -1 failed. */
int net_poll_resolve(uint8_t *out_ip);

#endif /* NET_H */
