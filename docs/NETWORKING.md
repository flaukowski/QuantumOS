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

## Phase 2 — IPv4, UDP, and DHCP (`kernel/src/net.c`)

On the same send/receive spine, phase 2 adds:

- **IPv4** with the 20-byte header and its one's-complement header
  checksum (`checksum16`).
- **UDP** (checksum left 0 — "not computed", which RFC 768 permits for
  IPv4 and SLIRP accepts).
- A **DHCP client**: a full DISCOVER → OFFER → REQUEST → ACK exchange.
  All four messages are broadcast (destination MAC `ff:…`, source IP
  `0.0.0.0`, destination `255.255.255.255`) with the BOOTP broadcast flag
  set, so no address or ARP resolution is needed first. The client tracks
  the transaction by a fixed `xid` and the DHCP magic cookie, parses the
  `msgtype`/`server-id` options, and on ACK records `yiaddr` as its lease.

Obtaining the lease is the phase-2 proof, because a successful ACK means
Ethernet TX *and* RX, the IPv4 header + checksum, UDP, and broadcast all
worked, both directions. The gate is `NET: DHCP lease 10.0.2.15` — the
exact address SLIRP's DHCP server hands out.

## CI gate additions

`make ci-smoke-net` now also asserts (with the timeout line as a hard
failure):

3. `NET: DHCP lease 10.0.2.15` — the DHCP exchange completed and the
   IPv4/UDP stack works end to end.

## Phase 3 — ICMP echo + DNS (`kernel/src/net.c`)

Phase 3 sends **unicast** IP (phases 1-2 were broadcast or gateway-only),
using the DHCP lease as the source address and an ARP-resolved
destination MAC:

- **ICMP echo** — an echo request (type 8) to the gateway 10.0.2.2 with
  an id/seq and a 32-byte payload, its ICMP checksum computed, sent to
  the gateway's resolved MAC. SLIRP answers ping to 10.0.2.2 internally,
  so this is a **fully self-contained** round trip that exercises unicast
  IPv4, the header checksum, and ICMP both directions. Gate:
  `NET: ping 10.0.2.2 reply received`.
- **DNS** — the capstone. ARP-resolve SLIRP's DNS proxy (10.0.2.3), send
  a DNS A-query for `example.com` (length-prefixed QNAME, recursion
  desired), and parse the response — skipping the question section,
  following name-compression pointers, and returning the first A record.
  SLIRP forwards the query to the runner's resolver, so a well-formed A
  record coming back proves the whole path: ARP → IPv4 → UDP → DNS, both
  directions. Gate: `NET: DNS example.com -> <a.b.c.d>` (any A record;
  the timeout line is a hard failure).

**A hobby OS that seeds its dice from quantum entropy can now resolve a
hostname over a network stack it built from the NIC driver up.**

The DNS gate is the one non-hermetic check in the suite: it depends on
the CI runner's external resolver (GitHub Actions runners reliably
resolve `example.com`). The ICMP gate is fully hermetic and proves the
unicast IP path on its own.

## Full CI gate (`make ci-smoke-net`)

1. `NET: rtl8139 up` — NIC found via PCI and brought up
2. `NET: ARP 10.0.2.2 is at MAC:` — link layer, both directions
3. `NET: DHCP lease 10.0.2.15` — IPv4 + UDP + broadcast
4. `NET: ping 10.0.2.2 reply received` — unicast IPv4 + ICMP
5. `NET: DNS example.com -> <ip>` — the full stack, a real hostname lookup

## Ring-3 access: `SYS_RESOLVE` + the shell's `nslookup`

The stack started as a kernel boot self-test; `SYS_RESOLVE` (#26) exposes
it to user programs. The design respects the one hard constraint: **a
syscall runs with interrupts disabled**, so it can never pump the NIC's
RX interrupt — it cannot do network I/O itself. Instead:

- The resident **`net` kernel thread** (IF=1, which already ran the
  self-test) owns all network I/O. After the self-test it enters
  `net_service_loop`, waking on each timer tick to service requests.
- `SYS_RESOLVE` is **non-blocking request/poll**: the first call posts a
  hostname into a shared slot (`resolve_state`, a volatile written last as
  the handshake flag) and returns `WOULD_BLOCK`; the net thread does the
  DNS lookup and fills the result; later calls poll — `WOULD_BLOCK` while
  pending, then the address, or EIO on failure. The shell loops with
  `yield` (heartbeating so a slow lookup can't get it watchdog-killed).
- It is **capability-gated**: `CAP_RESOURCE_DEVICE` over `DEVICE_ID_NET`
  (a synthetic id — the NIC's I/O base is dynamic), granted to `qsh`
  alone. The capless `ghost_test` proves the denial by attack every boot
  (`NETC: capless caller denied (EPERM)`).

The shell gains `nslookup <host>`. CI pipes `nslookup example.com` into
the shell and gates `qsh: example.com -> <a.b.c.d>` — a hostname resolved
**from ring 3** through the syscall, distinct from the boot self-test's
`NET:` line. A NIC-less boot reports no network honestly.

## Ring-3 UDP sockets: `SYS_UDP` (epic #80, `kernel/src/net_udp.c`)

`SYS_RESOLVE` asks the kernel to do one specific thing; `SYS_UDP` (#27)
generalizes the same worker spine into real sockets — user programs send
and receive **arbitrary datagrams**. It is op-multiplexed over a request
struct (`udp_req_t` in `user/usys.h`; the usys wrappers cap out at three
registers — the classic socketcall shape): `UDP_BIND` (port 0 =
ephemeral, from 49152 up), `UDP_SENDTO`, `UDP_RECVFROM`, `UDP_CLOSE`.
Everything is non-blocking (`WOULD_BLOCK` = poll again), and every op is
gated on the same `DEVICE_ID_NET` capability held by `qsh` alone — the
capless `ghost_test` proves the bind denial by attack every boot, NIC or
no NIC (`NETC: capless UDP bind denied (EPERM)`).

The concurrency design was adversarially attacked before implementation
(epic #80 records the findings); the load-bearing rules:

- **The net thread owns ALL NIC I/O.** `UDP_SENDTO` copies the payload
  into a kernel TX ring (the net thread runs under its own CR3 — it must
  never see a user pointer); the net thread drains it, ARP-resolving the
  next hop through a small **ARP cache** (gateway and DNS proxy warm
  after the self-test). The drain consumes an entry *completely* before
  advancing the ring's tail, so a syscall can never overwrite an entry
  mid-transmit.
- **Every received frame is offered to the socket demux first** via a
  single `net_rx()` wrapper — the only caller of `rtl8139_receive` — so
  no kernel wait loop (ARP/DHCP/ICMP/DNS) can destroy a user datagram.
  The demux validates ruthlessly: payload length comes from the UDP
  header (never the padded frame length), fragments are dropped, forged
  lengths can't overread the frame.
- **Socket slots have a three-state lifecycle** (`FREE → ACTIVE →
  CLOSING → FREE`): close and process-teardown only *mark* a slot; the
  net thread alone retires `CLOSING → FREE`, at the top of a wake, where
  it is by construction never mid-copy. A slot can't be rebound under a
  preempted demux. Per-socket RX rings are SPSC with free-running
  uint32 indexes; every multi-field publish ends with a compiler barrier
  and one volatile store.

The shell gains `udping <host>` — a **userspace DNS client**: it builds
the A-query in ring 3, `UDP_SENDTO`s it to SLIRP's proxy `10.0.2.3:53`,
`UDP_RECVFROM`s the raw reply (validating sender and txid), and parses
the answer itself. CI gates both `qsh: udp <N> bytes from 10.0.2.3:53`
(raw datagrams, both directions, through the socket API) and
`qsh: udpdns example.com -> <a.b.c.d>` (the ring-3 parse). SLIRP only
*forwards* DNS to the runner's resolver, so these gates share the
existing DNS gates' (reliable) runner-resolver dependency — no new risk
class.

## A TCP client: `SYS_TCP` + the shell's `http` (epic #82, `kernel/src/net_tcp.c`)

The capstone. `SYS_TCP` (#28) is a real, if minimal, TCP **client** — one
active-open connection at a time — built on the same net-thread spine.
The IF=1 net thread owns the entire state machine (CLOSED → SYN_SENT →
ESTABLISHED → the FIN dance → TIME_WAIT → CLOSED, plus an ERROR sink for
RST/timeout); syscalls only read/write the shared TCB under the publish
discipline and post one-shot request flags (`connect_req` / `close_req` /
`abort_req` — independent flags so a reaper's abort can never be clobbered
by a concurrent close). The op-multiplexed syscall exposes CONNECT / SEND
/ RECV (0 = EOF) / CLOSE / STATUS, all non-blocking WOULD_BLOCK polls,
capability-gated exactly like `SYS_UDP`.

The design was adversarially attacked before implementation (epic #82
records the findings). The load-bearing pieces the attack forced:

- **One-pass input.** A single segment carrying ACK + payload + FIN (both
  real servers piggyback the FIN on the last data segment) is processed
  as an ordered pipeline — validate/checksum/RST, then the ACK field,
  then in-order payload, then the FIN — with exactly one ACK reflecting
  the final `rcv_nxt`. Mutually-exclusive branches would drop the FIN and
  hang forever waiting for an EOF that already arrived.
- **Mandatory checksum, done right.** The segment length comes from the
  IP header's `total_len`, never the Ethernet frame length (a 54-byte
  control segment is padded to the 60-byte minimum; folding the pad into
  the checksum drops every ACK). The checksum runs once over a single
  running sum of pseudo-header + header + payload.
- **A window-update path.** The advertised window is the free receive-ring
  space; after the app drains the ring the net thread proactively
  re-advertises, instead of deadlocking on a peer that is waiting for a
  window it was last told was zero.
- **A terminal ERROR + net-thread-only retire.** A syscall never resets a
  net-thread-owned field; ERROR is sticky until an abort retires the TCB
  to a pristine CLOSED (the UDP three-state lesson), and the ephemeral
  port advances every open so a stale in-flight segment can't 4-tuple
  match a fresh connection.

The shell gains `http <host> [port]`: it resolves the name (`SYS_RESOLVE`
— two syscalls compose), connects, sends one `GET / HTTP/1.0` with
`Host:` and `Connection: close`, reads the response to EOF, and prints the
status line and byte count. **CI proves it two ways.** A hermetic gate
runs a loopback `python3 -m http.server` on the runner: SLIRP forwards a
guest connection to `10.0.2.2:PORT` to the host's `127.0.0.1:PORT`, so
`http 10.0.2.2 18080` drives a full three-way handshake, bidirectional
data, and FIN teardown against a real TCP peer with **zero external
network** — gated on `qsh: http 10.0.2.2 -> HTTP/1.[01] 200` and
`qsh: http 10.0.2.2: <N> bytes received`. Then `http example.com` is the
real-world capstone (`qsh: http example.com -> HTTP/1.[01] 200`), the same
non-hermetic runner-egress class as the `example.com` DNS gate.

## Source layout (after the transport split)

The stack started life as a single `kernel/src/net.c`. As the ring-3
transports landed it grew past 1900 lines, so it is now split by layer —
same code, three translation units plus a shared internal header:

- **`kernel/src/net.c`** — the control plane: Ethernet, ARP, IPv4, ICMP,
  DHCP, DNS, the `SYS_RESOLVE` state machine, and the boot self-test.
- **`kernel/src/net_udp.c`** — the ring-3 UDP sockets (epic #80): the
  socket table, the SENDTO tx ring, the `net_udp_*` syscall API, and the
  net-thread rx demux / closing-slot retire / tx drain.
- **`kernel/src/net_tcp.c`** — the ring-3 TCP client (epic #82): the
  single TCB and its net-thread state machine, plus the `net_tcp_*` API.
- **`kernel/include/kernel/net_internal.h`** — the shared internal
  surface: the `eth`/`ip`/`udp` wire structs (with `_Static_assert` size
  guards), the `htons`/`htonl`/`ip_eq` helpers, the netif globals, and the
  handful of prototypes one net TU calls in another. Not a public API;
  user-facing declarations still live in `kernel/include/kernel/net.h`.

The transports keep all their state `static` in their own file, so the
concurrency invariants (the volatile publish discipline on the UDP tx ring
and the TCP TCB) are unchanged — the split is a pure code move.

## Known limits / follow-ups (the honest boundary)

- **TCP is client only.** No listen/accept (no server), one connection at
  a time, stop-and-wait send (one outstanding segment ≤ MSS), in-order
  receive only (out-of-order segments are dropped; the peer retransmits),
  no congestion control, a shortened ~1 s TIME_WAIT (not 2 MSL), and no
  TLS. A complete, honest fetch-a-page client — not a general stack.
- UDP: 4 sockets, 4-deep rings, 1472-byte datagrams (no IP
  fragmentation), no UDP TX checksum (0 is legal for IPv4), no
  broadcast/multicast send.
- Single rtl8139, IPv4 only. Only `qsh` holds the network capability
  today; one kernel resolve and one TCP connection in flight at a time.
