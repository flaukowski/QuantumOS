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

/* Bring up the network stack over the NIC (no-op if no NIC). */
void net_init(void);

/* Boot self-test: ARP-resolve the user-net gateway and log the result.
 * Runs after interrupts are live (so the RX IRQ can fire) — invoked from
 * the idle loop once, not inline in early boot. Yields/halts while it
 * waits so the timer keeps ticking and the NIC IRQ is delivered. */
void net_selftest(void);

#endif /* NET_H */
