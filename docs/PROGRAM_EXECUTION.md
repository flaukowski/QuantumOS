# Program Execution: SYS_SPAWN & SYS_WAITPID

Epic #62, phase 3 (issue #65). The closing loop: boot → shell → load a
program **off the filesystem** → run it → collect its exit code. This is
what turns a fixed constellation of build-time services into a system
that runs programs on demand.

## Programs on the filesystem, not in the kernel

The service ELFs (ghostd, paradoxd, …) are embedded in the kernel image
and started at boot. `/bin/hello` is different: it is compiled, then
placed on the **initrd** (`build/x86_64/rootfs-stage/bin/hello`), and the
kernel loads it only when the shell asks. The Makefile stages a copy of
`rootfs/` plus the compiled `/bin` programs into `rootfs-stage/` and tars
that — so the kernel never carries `/bin/hello`'s bytes as a linked
symbol; they live in the archive and are found by path at spawn time.

## Syscalls

### `SYS_SPAWN` (21)

`rdi` = path (NUL-terminated, user memory). Looks up the initrd file,
loads its ELF into a fresh per-process address space through the same
`user_process_spawn_elf` the boot services use, and returns the new pid.

Spawning is **real authority** — a process that can start programs can
multiply — so unlike the VFS reads, `SYS_SPAWN` is capability-gated:
`CAP_RESOURCE_PROCESS` with `CAP_EXECUTE` over `SPAWN_RESOURCE_ID`,
declaratively granted to `qsh` alone (`grant_spawn`, re-minted on every
start). The capless `ghost_test` proves the denial by attack every boot
(`SPAWN: capless caller denied (EPERM)`). Returns ENOENT for an unknown
path, EIO if the ELF load fails.

### `SYS_WAITPID` (22)

`rdi` = target pid. Returns the exit code (0–255) once the process has
exited, `WAITPID_RUNNING` (256) while it lives, ENOENT for an unknown
pid. Non-blocking — the caller polls, yielding between tries (and
heartbeating, so a watchdog-monitored shell isn't killed mid-wait).

**The exit ledger.** The idle-loop reaper `memset`s a terminated PCB
long before a shell gets around to polling it, so the exit code can't
live only in the PCB. `sys_exit` records `(pid, code)` in a small kernel
ring (`EXIT_LEDGER_SIZE` = 16); `SYS_WAITPID` consults the ledger
newest-first (so a recycled pid still resolves to the exit the waiter is
after), then the live table. Single CPU, syscalls run interrupts-off, so
no locking is needed.

## Shell integration

`qsh` gains `run <path>`: spawn the program, print its pid, poll to
completion, and report the exit code. `/bin/hello` prints a greeting and
exits 42; `run /bin/hello` reports `qsh: pid N exited (code 42)`.

## CI gates (in `make ci-smoke` + Integration Test 1h)

The piped session ends with `run /bin/hello` before `exit`:

1. `HELLO: greetings from /bin/hello` — the program ran, loaded off the initrd
2. `exited (code 42)` — the shell collected the exit code (SYS_WAITPID)
3. `SPAWN: capless caller denied (EPERM)` — the capability gate, by attack

## Watchdog hardening (bundled from the phase-1 adversarial review)

A multi-agent adversarial review of the phase-1 shell surfaced three
kernel-framework defects that `SYS_SPAWN` makes more reachable (user-driven
spawning recycles pids at runtime). Fixed here:

- **`start_slot` grant ordering** — the reborn service is now spawned,
  granted its caps, and has its pid recorded all under one `cli` window,
  so the scheduler can never run a watchdog-reborn service before its
  console/quantum/spawn caps and `slot->info.pid` exist. Previously a
  timer preemption in that window could schedule a capless shell (console
  EPERM → suicide) or leak caps to a half-built, recyclable pid.
- **Recycled-pid teardown** — a per-slot generation counter
  (`process_get_generation`) is recorded at spawn and checked in
  `service_stop`, so the ~2 s-late watchdog can no longer `process_destroy`
  an innocent process that recycled the freed slot. This is the defect
  `SYS_SPAWN` most directly worsens.
- **Bounded console TX** — `console_write` caps its interrupts-off wait
  for the UART transmit register, so a wedged host-side serial reader
  drops a byte instead of freezing the whole kernel.

## Known limits / follow-ups

- No argv/environment yet — programs start at `_start` with no arguments.
- No `fork`; spawn always loads a fresh ELF (which suits a
  no-shared-memory microkernel).
- Exit ledger is 16 deep; a program whose exit is never waited on ages
  out of the ledger (harmless — `SYS_WAITPID` then returns ENOENT once
  the PCB is also reaped).
- `run` is foreground-only; no job control or `&`.
