/**
 * QuantumOS COM2 Serial Swarm Bridge implementation (ghostd phase 4, #51).
 *
 * Polled UART on 0x2F8 — the mirror of the COM1 boot console's write loop
 * in boot.S, but a distinct port and exposed to ring 3 as a capability
 * resource rather than a kernel-only log sink. COM1 is never touched here.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/com2_uart.h>
#include <kernel/boot.h>

/* 16550 register offsets from the UART base */
#define UART_THR   0   /* Transmit Holding Register (write, DLAB=0) */
#define UART_RBR   0   /* Receive Buffer Register  (read,  DLAB=0) */
#define UART_DLL   0   /* Divisor Latch Low        (DLAB=1) */
#define UART_IER   1   /* Interrupt Enable Register (DLAB=0) */
#define UART_DLH   1   /* Divisor Latch High       (DLAB=1) */
#define UART_FCR   2   /* FIFO Control Register (write) */
#define UART_LCR   3   /* Line Control Register */
#define UART_MCR   4   /* Modem Control Register */
#define UART_LSR   5   /* Line Status Register */

#define LSR_DATA_READY  0x01   /* a byte is waiting in the RBR */
#define LSR_THR_EMPTY   0x20   /* the THR can accept another byte */

#define LCR_DLAB        0x80   /* divisor-latch access bit */
#define LCR_8N1         0x03   /* 8 data bits, no parity, 1 stop bit */

static uint8_t com2_ready = 0;

static inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t io_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void com2_init(void) {
    /* Disable interrupts — this port is polled, mirroring the COM1 console. */
    io_outb(COM2_PORT_BASE + UART_IER, 0x00);

    /* 115200 baud: DLAB on, divisor 1, DLAB off + 8N1. */
    io_outb(COM2_PORT_BASE + UART_LCR, LCR_DLAB);
    io_outb(COM2_PORT_BASE + UART_DLL, 0x01);
    io_outb(COM2_PORT_BASE + UART_DLH, 0x00);
    io_outb(COM2_PORT_BASE + UART_LCR, LCR_8N1);

    /* Enable + clear the FIFOs (14-byte RX trigger). */
    io_outb(COM2_PORT_BASE + UART_FCR, 0xC7);

    /* DTR + RTS asserted; OUT2 set (harmless while polling). */
    io_outb(COM2_PORT_BASE + UART_MCR, 0x0B);

    com2_ready = 1;
    boot_log("COM2 serial swarm bridge online (0x2F8, polled, cap-guarded)");
}

uint32_t com2_write(const uint8_t *buf, uint32_t len) {
    if (!com2_ready || !buf) {
        return 0;
    }
    for (uint32_t i = 0; i < len; i++) {
        while (!(io_inb(COM2_PORT_BASE + UART_LSR) & LSR_THR_EMPTY)) {
            /* spin until the transmit holding register drains */
        }
        io_outb(COM2_PORT_BASE + UART_THR, buf[i]);
    }
    return len;
}

uint32_t com2_read(uint8_t *buf, uint32_t len) {
    if (!com2_ready || !buf) {
        return 0;
    }
    uint32_t n = 0;
    while (n < len && (io_inb(COM2_PORT_BASE + UART_LSR) & LSR_DATA_READY)) {
        buf[n++] = io_inb(COM2_PORT_BASE + UART_RBR);
    }
    return n;
}
