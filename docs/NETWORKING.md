# Networking: PCI, the RTL8139 NIC, and the link layer

Epic #73, phase 1. QuantumOS booted, ran a shell, kept a persistent
filesystem — but could not speak to a network. This phase adds the
foundation: PCI device discovery, a real NIC driver, and enough of the
link layer to prove packets flow both directions against QEMU's built-in
user-mode network (SLIRP).

## Why prove it with ARP against SLIRP

QEMU's `-netdev user` gives the guest a private 10.0.2.0/24 network whose
gateway (10.0.2.2), DHCP server, and DNS (10.0.2.3) always respond — no
host privileges, no external network, fully deterministic. That makes it
perfect for a headless CI gate. Phase 1's proof is an **ARP request for
the gateway**: if SLIRP's reply comes back and we parse it, then PCI
enumeration, the driver's transmit path, the receive interrupt, and ARP
parsing all work — the entire link layer, both directions, through the
real driver.

## PCI enumeration (`kernel/src/pci.c`)

Just enough PCI to find one device: the classic `0xCF8`/`0xCFC`
configuration port pair, a scan of bus 0 for a vendor:device match
(`10EC:8139`), and reads of BAR0 (the I/O base), the interrupt line
(config offset `0x3C`), and the command register (to set the
bus-master + I/O-space enable bits so the NIC can DMA and answer port
I/O). This is the OS's first device-discovery mechanism; every future
PCI device rides the same three functions.

## The RTL8139 driver (`kernel/src/rtl8139.c`)

QEMU's `-device rtl8139` — the classic hobby-OS NIC because its receive
path is a single **linear ring buffer** rather than a descriptor ring.

- **Bring-up**: power on (`CONFIG_1` = 0), soft-reset and poll (bounded)
  until the reset bit clears, read the 6-byte MAC, program the RX buffer
  base, set the RX config (accept broadcast + physical-match + multicast,
  `WRAP`, 8 KiB buffer), enable the ROK/TOK interrupts, and enable the
  receiver + transmitter.
- **Receive** is interrupt-driven. The chip DMAs each frame — prefixed by
  a 4-byte `{status, length}` header — into the ring and advances its
  write pointer; the IRQ handler walks from our read cursor, strips the
  header and the trailing 4-byte CRC, and pushes each frame into a small
  kernel queue, then advances `CAPR` (with the chip's mandatory
  `CAPR = offset − 16` quirk). The `WRAP` bit means the chip never splits
  a frame at the buffer's end (it may write up to a frame past it), so
  the buffer is oversized and no manual wrap-stitching is needed.
- **Transmit** uses the 4 legacy TX descriptors round-robin: copy the
  frame to a per-descriptor bounce buffer (in the low identity map, so
  its virtual address *is* its DMA address), program the descriptor's
  address and length, and wait (bounded) for the `TOK` bit. Runt frames
  are zero-padded to the 60-byte Ethernet minimum; the chip appends the
  CRC.
- Every hardware wait is **bounded** (the `console_write`/ATA lesson).

The received-frame queue is the `console.c` ring pattern: an IRQ producer
(IF=0) and a consumer that takes an interrupt-save guard, so the network
layer drains frames without racing the handler.

## The link layer (`kernel/src/net.c`)

Ethernet + ARP, with `htons`/`ntohs` on every field wider than a byte
(network order is big-endian, x86 is little-endian). The boot self-test
sends a broadcast ARP request for 10.0.2.2 and waits (bounded) for the
reply, matching it by opcode and sender IP before logging the resolved
MAC.

**Where it runs matters.** The NIC's IRQ only comes alive after
`interrupt_enable_all()` at the very end of `kernel_init`, so the
self-test cannot run inline in early boot. It runs in a dedicated
**kernel thread** (`net`) created at scheduler init: with interrupts
enabled, it sends the ARP and then `hlt`s between polls — sleeping until
the next interrupt (the timer, or the point: the NIC's RX IRQ). A tight
IF-blind spin there would deadlock, since the RX IRQ could never preempt
it to fill the queue.

## The dynamic IRQ

Unlike the timer/keyboard/COM1 (fixed IRQs 0/1/4), the NIC's line is
assigned by PCI at runtime (read from config space). So it can't be a
compile-time `case` in the IRQ dispatch: `irq_handler`'s default case
routes the interrupt to the driver when the number matches
`rtl8139_irq_line()`, and `kernel_init` unmasks that line only when a NIC
is present.

## CI gate: `make ci-smoke-net` (new required `networking` job)

Boots with `-netdev user -device rtl8139` and asserts:
1. `NET: rtl8139 up` — the NIC was found via PCI and brought up.
2. `NET: ARP 10.0.2.2 is at MAC:` — an ARP request reached SLIRP and its
   reply came back through the RX interrupt and parsed.

Live proof: the resolved gateway MAC is `52:55:0A:00:02:02` — exactly the
address SLIRP derives for 10.0.2.2, so the reply is genuine.

The default `-kernel` boot attaches no rtl8139; the driver logs
`NET: no rtl8139 NIC found — networking disabled` and continues
unchanged. The default `make ci-smoke` double-side-gates that honest
degrade (no-NIC line present, `rtl8139 up` absent).

## Known limits / follow-ups (the honest boundary)

- **No IP stack yet** — phase 1 is link-layer only. Phase 2 adds IPv4 +
  UDP + a DHCP client (gate: obtaining the SLIRP `10.0.2.15` lease);
  phase 3 adds ICMP + DNS (capstone: a DNS query round-trip). **No TCP**
  is planned — UDP/DHCP/DNS/ICMP is a complete, useful stack, and TCP is
  the honest follow-up.
- Single rtl8139, IPv4 only, kernel-internal (a ring-3 socket API is a
  later epic).
- The self-test uses the SLIRP-assumed source IP 10.0.2.15 before DHCP;
  SLIRP answers ARP regardless of the requester's address, so this is
  fine for phase 1 and becomes a real lease in phase 2.
