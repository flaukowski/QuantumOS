# Ring-3 programs: the syscall ABI and the libq runtime

This is the contract for writing a user program that runs on QuantumOS —
the stable syscall interface every ring-3 process sees, and `libq`, the
freestanding C runtime (heap + libc-lite + printf) that programs link
against. Native app "citizens" are built on this; it is also the substrate
a future real-binary port (e.g. a Rust `no_std` target) would target.

## The program model

A user program is a freestanding, statically-linked ELF loaded at a fixed
address. It has no libc and no CRT: it defines its own entry point
`void _start(void)` (the linker script's `ENTRY(_start)`), does its work
through syscalls, and ends by calling `exit_(code)`. There is no return to
a caller — `_start` never returns (a trailing `for (;;) {}` after `exit_`
keeps the compiler happy).

Two kinds of program exist:

- **Kernel-embedded services** (init, ghostd, paradoxd, swarm_svc, qsh, …):
  linked into the kernel image and started at boot with capabilities.
- **Filesystem programs** (`/bin/hello`, `/bin/args`, `/bin/libqtest`):
  ride the initrd and are started from the shell with `run /bin/<name>`
  through `SYS_SPAWN`. Their exit code comes back to the shell via
  `SYS_WAITPID`. A spawned program reads its argument vector with
  `get_args()`.

To add a filesystem program: drop `user/<name>.c` in, add `<name>` to
`USER_PROGS` (and to `INITRD_BIN_PROGS` for it to appear under `/bin`) in
the Makefile. The build compiles it with the `USER_CFLAGS` (freestanding,
`-O2`, `-Werror`) and links `libq.a` after it automatically.

## Memory layout

Each process gets a private address space in `[0x40000000, 0x80000000)`:

| region      | address                     | notes                          |
|-------------|-----------------------------|--------------------------------|
| image       | `0x40000000` (`USER_VBASE`) | text, rodata, data, bss        |
| args page   | `0x40090000`                | read-only packed argv          |
| user stack  | `0x40100000` down, 16 KiB   | `USER_STACK_TOP`, 4 pages      |

The ELF loader maps each `PT_LOAD` segment's full `p_memsz` (bss zeroed),
so a static bss array — like the libq heap arena — is backed at exec. The
loader does **no** overlap check, so the total image (including the heap
arena) must fit below the args page. `user/user.ld` enforces this with a
mandatory `ASSERT(_end <= 0x40080000)`, leaving ≥64 KiB of margin.

## The syscall ABI

Syscalls use `int 0x80`: the number in `rax`, up to three arguments in
`rdi`, `rsi`, `rdx`, and the result in `rax`. `user/usys.h` provides the
raw wrappers (`usys0`..`usys3`) and typed helpers over them; it is the
canonical, stable definition. The number space (frozen; new syscalls
append):

| #  | name           | helper                    | purpose                          |
|----|----------------|---------------------------|----------------------------------|
| 1  | SYS_WRITE      | `write_str`               | write a framed console line      |
| 2  | SYS_GETPID     | `getpid`                  | caller pid                       |
| 3  | SYS_YIELD      | `yield`                   | cooperative reschedule           |
| 4  | SYS_EXIT       | `exit_`                   | terminate with a code            |
| 5  | SYS_TICKS      | `ticks`                   | monotonic tick count             |
| 6–7| SYS_SEND/RECV  | `send_msg`/`recv_msg`     | capability-routed IPC            |
| 8–9| SYS_HEARTBEAT… | `heartbeat`/`svc_restarts`| watchdog liveness / rebirth      |
| 10 | SYS_QRAND      | `qrand_fill`              | quantum-seeded randomness (cap)  |
| 11 | SYS_SEND_TO    | `send_to`                 | targeted capability IPC          |
| 12 | SYS_COM2       | `com2_read/write_bytes`   | swarm-bridge UART (cap)          |
| 13 | SYS_QSEED      | `qseed_value`             | boot qseed provenance (cap)      |
| 14 | SYS_FIELD_SNAP | `field_snapshot`          | framebuffer field viz            |
| 15 | SYS_CONS       | `cons_read/write`         | raw console I/O (cap)            |
| 16 | SYS_SYSINFO    | `sysinfo`/`sysinfo_quiet` | ps/mem/time/quiet/peer introspection |
| 17–20 | SYS_OPEN…READDIR | `open_`/`read_`/`close_`/`readdir_` | initrd + RAM overlay read |
| 21–22 | SYS_SPAWN/WAITPID | `spawn_`/`waitpid_`  | exec a /bin ELF, reap it         |
| 23–25 | SYS_FWRITE/UNLINK/SYNC | `fwrite_`/`unlink_`/`sync_` | RAM overlay writes + disk persist (cap) |
| 26 | SYS_RESOLVE    | `resolve_`                | DNS hostname → ip (cap)          |
| 27 | SYS_UDP        | `udp_`                    | ring-3 UDP sockets (cap)         |
| 28 | SYS_TCP        | `tcp_`                    | ring-3 TCP client (cap)          |
| 29 | SYS_IMPRINT    | `imprint_`                | store into a kernel field region (cap) |
| 30 | SYS_RECALL     | `recall_`                 | ranked associative recall (cap)  |
| 31 | SYS_FIELD_INFO | `field_info_`             | read-only field enumeration (cap) |
| 32 | SYS_AUDIT      | `audit_`                  | read the capability authority ledger (uncapped RO) |
| 33 | SYS_MANIFEST   | `manifest_`               | read the per-pid intent manifests (uncapped RO) |
| 34 | SYS_CAP_DERIVE | `cap_derive_`             | delegate a narrowed capability to a sub-agent (cap) |

**Cross-ring capability delegation (epic #137).** `SYS_CAP_DERIVE` lets a
citizen holding `CAP_GRANT` hand a strictly-**narrowed** slice of one of
its own capabilities to a sub-agent — "an agent hands a narrowed intent to
a sub-agent." The caller names its parent capability by
`(resource_type, resource_id)` (never a ring-3 handle, so no handle
forgery) and the narrowed permission subset; `CAP_GRANT`/`CAP_REVOKE` are
refused in the handed permissions, so delegation is provably **one-hop**
(the sub-agent cannot itself re-delegate). The kernel bounds the derive by
AND reflects it in the intent manifest (epic #135): the caller may only
delegate a resource it is itself declared to touch (a transitive outer
bound, `manifest_check` on the delegator), and the derive **extends the
recipient's manifest** (`manifest_grant`) so the delegated cap is actually
usable — a delegated cap whose resource is absent from the recipient's
manifest would otherwise be denied. The target must be an IPC peer of the
caller (so a `CAP_GRANT` holder can inject a cap only into a process it was
explicitly wired to), a live ring-3, non-self, manifest-bound, non-monitored
process. The op is idempotent (a covering cap already held → success without
minting). Delegation narrows PERMISSIONS and EXPIRY, not the resource
(region granularity is the unit). When the delegator dies, cascade
revocation kills the derived cap — the un-fakeable proof that authority was
*delegated* (a static grant survives an unrelated process's death). Proven
every boot by the `delegation-test` → `subagentd` demo.

**The intent manifest + spawn quota (epic #135).** Above raw
capabilities sits a per-pid **intent manifest**: the allow-set of
`{resource_type, resource_id}` a citizen was *declared* to touch, built
from the same grant flags that mint its caps and bound in the same
interrupt-off window (`manifest_bind`), checked after `cap_find` at every
capability-gated syscall (`manifest_check` → `AUDIT_MDENY` on a held cap
that exceeds declared intent). The manifest also carries the **first
enforced quota**: `spawn_max` limits successful `SYS_SPAWN`s per
incarnation (checked before any spawn side effect, charged only on
success → `AUDIT_QUOTA` on refusal). A per-pid **`cpu_limit`** (epic
#144) is the second enforced quota: `cpu_ticks` counts timer ticks the
process is scheduled in, and once it exceeds a declared `cpu_limit` the
kernel **terminates** the process from the timer tick — a busy-spin
runaway that ignores cooperative scheduling cannot hog the machine. The
kill is guarded by `(cs & 3)` so a process caught mid-syscall in ring 0 is
skipped and caught the next tick it is back in ring 3 (the cumulative-tick
condition is a latch); it records an `AUDIT_CPUKILL` ledger entry and the
pid vanishes from the process/manifest tables. Enforcement is **opt-in**
(`cpu_limit == 0` = unlimited), so every shipped long-runner (qsh, ghostd,
…) is untouched — only a citizen that declares a finite budget is
eligible (proven every boot by the `cpu-hog` citizen). On the
shipped system caps and manifests are minted from the same grants, so the
intent check refuses nothing today — its value is inspectability
(`SYS_MANIFEST`) and the outer bound capability delegation will be checked
against; the spawn quota is the non-vacuous enforcement now (proven every
boot by the `quota-test` citizen). A boot self-test drives the deny path
live so it cannot ship as dead code. IPC caps stay OUTSIDE the manifest
(pair-wise runtime wiring, not declarative intent). The ledger records
the full **revocation lifecycle** with honest kinds: `AUDIT_REVOKE` for
an explicit `cap_revoke` (+ its cascade), `AUDIT_REAP` for slots freed
because their owner died (the reaper's cleanup + cascade) — a verifier
never mistakes garbage collection for exercised revocation authority.
Each cascade-freed delegated cap records the *recipient's* pid (the
holdings that changed), proven every boot by the delegation demo: the
exited delegator's own FIELD cap REAPs under its pid, and subagentd's
derived READ cap REAPs under subagentd's.

**The kernel holographic field (epic #95).** `SYS_IMPRINT` stores a
byte pattern (≤ 64 bytes, with a Q15 importance) into one of the
kernel's fixed field REGIONS; `SYS_RECALL` scores every live slot in a
region against a probe — Q15 cosine of centered unit wavefronts times
energy, one bounded integer pass, no floats — and returns the top-K
`(slot, score)` rankings plus the winner's stored bytes: a ~15%-
corrupted probe recovers the exact original. Both calls take request
structs (`field_imprint_req_t` / `field_recall_req_t` +
`field_recall_out_t` in `usys.h`; layouts `_Static_assert`-pinned
against the kernel twins) whose `region` field must be named EXACTLY by
the caller's `CAP_RESOURCE_FIELD` capability — granted declaratively to
services (`grant_field` + `field_region`, scrubbed and re-minted on
every restart so a reborn or successor service never inherits its
predecessor's memories), never to spawned `/bin` programs. A capless
caller is denied `-4` before the kernel reads its request; a
wrong-region request by a cap holder is also `-4` (that comparison IS
the isolation boundary). Recall's retrieval reinforcement (an energy
boost on the winner) only happens when the caller also holds the write
right — a read-only capability never mutates. A degenerate
all-equal-bytes probe returns `n=0` (success); as a pattern it is `-1`
EINVAL. This is the RANKED holographic memory kind; `ghostd`'s
iterative attractor field is a deliberately different, living memory
that stays in ring 3.

**Conventions.** A non-negative return is success (often a count or fd);
a negative return is an errno: `-4` EPERM (no capability authorises it),
`-6` ENOENT, `-5` EIO, `-1` EINVAL, `-2` EFAULT. Non-blocking calls
(`SYS_RESOLVE`, `SYS_UDP`, `SYS_TCP`) return `-11` (WOULDBLOCK) while
pending — poll again after a `yield`. Privileged operations require a
capability the process was granted at spawn; a program without it gets
EPERM rather than the resource. `SYS_WRITE` frames each call as
`[user pid=N] <text>\r\n` and truncates at 127 bytes.

## The libq runtime (`user/libq/`)

`#include "libq/libq.h"` — the only include a program needs; it also pulls
in `usys.h`. `libq.a` is linked automatically. The linker pulls only the
archive members a program references, so a program that never allocates
gets neither the heap arena nor printf in its image.

- **mem** (`mem.c`): `memcpy`, `memmove`, `memset`, `memcmp`. These are
  the functions gcc itself emits calls to for struct/array copies, so they
  must exist as real symbols. `mem.c` and `str.c` are compiled with
  `-fno-tree-loop-distribute-patterns -fno-builtin` so `-O2` cannot rewrite
  a copy/fill loop into a call to the function being defined (a silent
  infinite recursion); a pre-boot objdump gate enforces this.
- **str** (`str.c`): `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`,
  `strchr`.
- **heap** (`heap.c`): `malloc`, `free`, `calloc`, `realloc` over a 64 KiB
  per-process bss arena (override with `-DLIBQ_HEAP_SIZE`). First-fit
  explicit free list with boundary tags and immediate bidirectional
  coalescing; every payload is 16-byte aligned; `free`/`realloc` gate the
  pointer (bounds, alignment, size/footer agreement) so a stray or
  double-freed pointer fails safe. No `SYS_BRK` yet — the arena is fixed;
  the growable-heap path is deferred but the API is stable across the swap.
- **printf** (`printf.c`): `printf`, `vprintf`, `snprintf`, `vsnprintf`.
  Integer-only (`%d %i %u %x %X %s %c %p %%`, the `0` flag, numeric width,
  `l`/`ll`) — no floating point, because the user ABI is `-mno-sse` and a
  float conversion would drag in soft-float. `snprintf`/`vsnprintf` are
  pure and follow C99 truncation. `printf`/`vprintf` format into a 128-byte
  stack buffer and hand the line to `SYS_WRITE`, so printf is
  **line-oriented** (one framed line per call, ~127-char cap), not
  byte-accurate stream output.
- **fx** (`fx.c`): fixed-point math — `fx_sin`/`fx_cos` (phase is a uint32
  "turn", the full circle is 2^32; return Q15, a value `v` meaning `v/32768`)
  and `fx_isqrt` (integer floor sqrt). QuantumOS saves no FPU/SSE state across
  a context switch, so a preemptible ring-3 program must not use float/double;
  citizens do their trig and roots through `fx`, exactly as `ghostd` does.

`user/libqtest.c` (`/bin/libqtest`) exercises the whole runtime at `-O2` in
ring 3 and prints `LIBQ: self-test OK`; the ci-smoke and integration boot
gates assert it, so a regression — including a reintroduced self-recursion
trap — turns CI red.

## Native app citizens

Native C "citizens" embody the constellation's apps on top of libq (the
`ghostd`/`paradoxd` pattern, now with a heap and printf). The first is
`user/consciousnessd.c` (`/bin/consciousnessd`) — the essence of
`consciousness-core`: a field of coupled Kuramoto phase oscillators
synchronizes, the order parameter `r` climbs from incoherent toward locked,
and `r` maps to a consciousness verdict (Dormant → Transcendent). It runs
entirely in `fx` fixed-point; the mean-field update `K·r·sin(ψ−θ)` expands to
`(K/N)·[Σsin·cosθ − Σcos·sinθ]`, so the `r` and mean-phase `atan2` cancel out
of the drive and are needed only to report `r`. A ci-smoke gate asserts the
field actually synchronizes (`r` past 0.8 → `CONSCIOUSNESS EMERGED`), not just
that the program ran.

`user/kannakad.c` is the essence of `kannaka-memory` — and since epic #95
phase 2, a **kernel-embedded service and the kernel field's reference
client**. Its holographic ranked-recall math (one resonance pass scoring
every stored memory by `similarity × energy` — not a search) was promoted
INTO the kernel as `SYS_IMPRINT`/`SYS_RECALL`; kannakad now exercises that
surface rather than carrying a private store. Declared with
`grant_field = 1, field_region = 1`, so the service framework scrubs and
re-mints its region-1 `CAP_RESOURCE_FIELD` cap on every start. At boot it
imprints seven importance-weighted seeds, recalls a byte-corrupted probe
back to the **exact stored text**, and proves retrieval reinforcement
raised the winner's score through the syscall's write-side contract (the
old in-process dream/hallucination pass died with the private store —
consolidation belongs to the kernel field's future). Where `ghostd` is a
single-basin binary attractor that relaxes onto one pattern, the kernel
field kannakad speaks to is the ranked, importance-weighted cousin. A
ci-smoke gate asserts all three behaviours (`RESONANCE VERIFIED`).

`user/quantumd.c` is the essence of `kannaka-quantum`, and — unlike the two
above — a **kernel-embedded service**, not a `/bin` program. `SYS_QRAND` and
`SYS_QSEED` are capability-gated (a capless `/bin` caller gets EPERM by
design), so quantumd is declared with `grant_quantum_pool = 1` in
`user_quantum_demo_init()`; the service framework mints its quantum-pool read
cap on every start. It draws **real collapse-derived entropy** — a quantum
coin, a rejection-sampled die, a qrng readout — and performs recall as
**amplitude amplification**: candidate resonance scores are amplitude-encoded,
Grover-amplified in closed form (`P_target = sin²((2m+1)·asin(√p))`, via
`fx_asin`/`fx_sin`), then sampled from the pool; the amplified argmax must
agree with the classical argmax (`QUANTUM VERIFIED`). It refuses to fall back
to a software PRNG — if the pool is denied it fails loud, honouring
kannaka-quantum's entropy discipline.

`user/qtop.c` (`/bin/qtop`) is the essence of `kannaka-tui`: a live system
dashboard. The original is a full-screen ratatui UI, but raw console control
(`cons_write`) needs the console-device capability only qsh holds, so qtop is
an honest **snapshot** dashboard built from the uncapped `SYS_SYSINFO` surface
and printed as framed lines — a memory gauge from the real frame allocator, the
wall clock, a decorative resonance-field wave (`fx_sin`, phase-shifted by
uptime), and the live process table. It renders once and exits — a `top` for
QuantumOS. A ci-smoke gate asserts `DASHBOARD RENDERED`.

`SYS_SYSINFO` also carries the uncapped bufferless config queries a coupling
service reads: `SYSINFO_QUIET` (booted `quiet`?), `SYSINFO_PEER` (arg 2 =
index → the packed `peer=` IP at that index, 0 out of range), and
`SYSINFO_PEER_COUNT` (how many peers `peer=` configured — 1 for a 2-VM
society, up to `MAX_PEERS` for the N-way mean-field society of epic #139).
These name no authority; they only report boot config.

Together the four citizens map the constellation onto QuantumOS:
consciousness-core → consciousnessd, kannaka-memory → kannakad,
kannaka-quantum → quantumd, kannaka-tui → qtop.
