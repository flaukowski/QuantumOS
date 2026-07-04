/**
 * QuantumOS COM2 Serial Swarm Bridge (ghostd phase 4, #51; epic #47)
 *
 * A minimal polled UART on the second serial port (0x2F8), kept
 * completely separate from the COM1 boot console (early_console_write in
 * boot.S, port 0x3F8). COM2 carries the swarm-bridge frame protocol
 * spoken by the ring-3 swarm_svc; the kernel only provides the raw byte
 * pipe, guarded as a capability resource so a ring-3 service reaches it
 * only when it holds a CAP_RESOURCE_DEVICE capability over DEVICE_ID_COM2.
 *
 * No interrupts, no buffering: init once, then poll the Line Status
 * Register for TX-drain / RX-ready exactly like the COM1 console does.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef COM2_UART_H
#define COM2_UART_H

#include <kernel/types.h>

/* I/O base of the second UART. Also used as the capability resource_id so
 * the guarded resource is self-identifying (CAP_RESOURCE_DEVICE / 0x2F8). */
#define COM2_PORT_BASE 0x2F8
#define DEVICE_ID_COM2 COM2_PORT_BASE

/* Bring up COM2: 115200 8N1, FIFOs on, interrupts off (polled). Idempotent
 * and safe to call once at boot. Never touches COM1. */
void com2_init(void);

/* Write `len` bytes, polling the THR-empty bit before each byte so none are
 * dropped. Returns the number written (== len). */
uint32_t com2_write(const uint8_t *buf, uint32_t len);

/* Drain up to `len` bytes currently available in the receive register
 * (non-blocking: stops as soon as the Data-Ready bit is clear). Returns the
 * number of bytes read (0 if none waiting). */
uint32_t com2_read(uint8_t *buf, uint32_t len);

#endif /* COM2_UART_H */
