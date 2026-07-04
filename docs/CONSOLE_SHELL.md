# Console Input & the qsh Shell

Epic #62, phase 1 (issue #63). This document describes the interactive
console — the OS's first *input* path — and `qsh`, the ring-3 shell built
on it.

## Why

Until this phase, everything QuantumOS ever did was decided at build
time: it booted a fixed constellation of services, ran its gates, and
idled. Output existed (the COM1 boot console); input did not. The
console input path plus a shell turn the demo kernel into a system a
human — or CI — can actually sit at: inspect live state, exercise
capabilities, and watch supervision work.

## Console input path (`kernel/src/console.c`)

One 1 KiB ring buffer, two interrupt-driven producers:

- **COM1 RX (IRQ4)** — bytes typed into the serial console: QEMU
  `-serial stdio`, the qBraid watch window, or CI's piped stdin.
- **PS/2 keyboard (IRQ1)** — scancode set 1 translated to ASCII (US
  layout, shift tracked; extended `0xE0` keys ignored for now), for
  interactive/graphical boots.

The COM1 *transmit* path is untouched — it remains `early_console_write`
in `boot.S`. `console_write()` adds a raw byte sink (no `[user pid]`
prefix) so a shell can own its own line discipline; every write runs
with interrupts disabled so a timer-tick log line can never split it.

### Public API (`kernel/include/kernel/console.h`)

| Function | Purpose |
|---|---|
| `console_init()` | Program COM1 line settings, rescue any byte already received, enable the RX interrupt, reset PS/2 state |
| `console_com1_irq()` | IRQ4 body: drain received bytes into the ring |
| `console_kbd_irq()` | IRQ1 body: translate one scancode, push ASCII |
| `console_read(buf, len)` | Non-blocking drain of buffered input (0 if none) |
| `console_write(buf, len)` | Raw, interrupt-atomic COM1 output |

`DEVICE_ID_CONSOLE` (= `0x3F8`, the port base) identifies the console as
a capability resource, the same self-identifying convention as
`DEVICE_ID_COM2`.

### The init-ordering subtlety

CI pipes shell input from t=0 — QEMU delivers the first byte into the
receiver long before the kernel reaches `console_init()`. Two rules keep
every byte alive:

1. **Never clear (or even toggle) the FIFOs.** QEMU forces a FIFO clear
   on any change of the FIFO-enable bit, and QEMU refills the receive
   register asynchronously the moment the guest drains it, so there is
   *no* point in the init sequence where a clear is provably safe (a
   real byte was lost to exactly this race during development: `help`
   arrived as `hep`). Per-byte RX interrupts are ample for a console.
2. **Drain before enabling the interrupt.** Any byte already waiting is
   pushed into the ring first; a byte that lands between the drain and
   the IER write still raises the IRQ, because Data-Ready is
   level-evaluated when IER changes.

The smoke test gates this with the *input integrity* check: the first
piped command must execute intact (`qsh: commands: help`).

## Syscalls

### `SYS_CONS` (15)

`rdi` = op (`SYS_CONS_READ`/`SYS_CONS_WRITE`), `rsi` = buffer,
`rdx` = length. Raw console I/O, moved through a bounded kernel bounce
buffer (`CONS_MAX_BYTES` = 256) and clamped to the caller's mapped user
half. Gated on a `CAP_RESOURCE_DEVICE` capability over
`DEVICE_ID_CONSOLE` — `CAP_READ` to read, `CAP_WRITE` to write, EPERM
without. Reads are non-blocking (0 when nothing is buffered). Only
`qsh` is granted the console, and the capless `ghost_test` proves the
denial by attack every boot (`CONS: capless caller denied (EPERM)`).

### `SYS_SYSINFO` (16)

`rdi` = op (`SYSINFO_PS`/`SYSINFO_MEM`), `rsi` = buffer, `rdx` = length.
Kernel-formatted, read-only introspection text — uncapped like
`SYS_GETPID`/`SYS_TICKS`, because it names no authority:

- `SYSINFO_PS` — one `PS: <pid> <name> <STATE>` line per live process,
  formatted by `process_format_ps()` in `process.c` (which owns the
  table). The kernel formats so the user side needs no struct ABI.
- `SYSINFO_MEM` — one `MEM: heap free=<n> bytes, frames free=<a>/<b>`
  line from the kernel heap and physical frame allocator.

Output is bounded by `SYSINFO_MAX_BYTES` (1024); the PS formatter stops
at whole rows.

## qsh (`user/qsh.c`)

A ring-3 user-process service, watchdog-monitored like every other
citizen. Declarative grants (re-minted on every start): the console
device capability and a quantum-pool read capability. One IPC send-cap
each way wires it to `ghostd`.

Line discipline lives in the shell, not the kernel: qsh echoes, handles
backspace, assembles lines. Every logical output line is written with a
single `SYS_CONS` call so tick logs never split it.

Builtins: `help`, `echo <text>`, `ps`, `free`, `uptime`, `pid`,
`qrand`, `qseed`, `ghost` (STATUS query to `ghostd` over capability
IPC), `clear`, `exit`.

`exit` is a supervised death: the shell terminates, its heartbeat goes
silent, and the service watchdog restarts it (~2 s); the reborn shell
introduces itself with `QSH: reborn (restart=N)`. That banner is the
merge-gate proof that the operator surface survives its own crash.

## CI gates (all in `make ci-smoke` and the Integration job)

The smoke test pipes `help ps free uptime ghost qrand exit` into QEMU's
stdin and asserts, from the same boot:

1. `QSH: QuantumOS interactive shell ready` — shell up with its console cap
2. `qsh: commands: help` — first piped command intact (input integrity)
3. `qsh RUNNING` — `ps` shows the shell observing itself in the table
4. `MEM: heap free=` — live memory stats
5. `qsh: ghost R=` — field status fetched from ghostd over IPC
6. `qsh: uptime ` and `qsh: qrand ` — kernel ticks + quantum-pool draw
7. `CONS: capless caller denied (EPERM)` — capability gate, by attack
8. `QSH: reborn` — watchdog restarted the shell after `exit`

## Known limits / follow-ups

- Serial-first: shell output is not yet mirrored to the VGA text screen
  or framebuffer console (follow-up: a proper text console layer).
- Peer IPC capabilities (qsh ↔ ghostd) are not re-minted on watchdog
  restart — only declared *resource* caps are. A reborn shell keeps its
  console but loses `ghost` until this service.c limitation is fixed
  (shared with the other demo peers).
- Extended PS/2 keys (arrows, etc.) are swallowed; no line history.
- Next phases of epic #62: an embedded initrd + read-only VFS (`ls`,
  `cat`, issue #64), then `SYS_SPAWN`/`SYS_WAITPID` so the shell can run
  programs from the filesystem (issue #65).
