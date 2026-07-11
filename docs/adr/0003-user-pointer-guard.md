# 3. Universal user-pointer copy guard at the syscall boundary

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Syscalls run cli'd, in ring 0, in the caller's address space (ADR-0001). Every
user-supplied pointer a handler dereferences is therefore a kernel-context
memory access, and this kernel has no fault-fixup tables: `contain_user_fault`
treats any ring-0 fault as fatal and `boot_panic`s the machine. The original
guard, `in_user_range()`, checked only that an address fell inside
`[USER_VBASE, 0x80000000)` — never whether it was mapped. But a user process
maps only a sparse sliver of that 1 GB: an 8-page code window, one data page,
one read-only argv page, and 4 stack pages (kernel/include/kernel/vmspace.h:22-27).
Issue #158: any ring-3 process could halt the whole OS by handing any syscall
an in-range but unmapped (or mapped read-only, for copy-out) pointer. This was
a universal, unprivileged, one-line denial of service against every citizen at
once.

The page-table layout makes a naive fix dangerous. Each process's PDPT[0]
points at the shared boot page directory — the identity map of the low 1 GB in
supervisor-only 2 MB PS pages (kernel/src/vmspace.c:54-60) — while the private
user half lives in PDPT[1] as 4 KB pages only. A page walker with no PS-bit
handling that wanders below `USER_VBASE` would misread a 2 MB frame as a page
table and could *falsely pass*, converting the DoS into an escalation.

## Decision

Validate every user span against the caller's own page tables before touching
it, and return EFAULT instead of ever letting ring 0 fault.

**The core check** is `vmspace_user_ok(pml4, uvaddr, len, need_write)`
(kernel/src/vmspace.c:105-155), layered strictly:

1. `len == 0` is trivially ok — this also guards the `uvaddr+len-1` underflow
   (vmspace.c:106-108); an overflow-wrapping `uvaddr+len` is rejected (:109-111).
2. **Range confinement to `[USER_VBASE, 0x80000000)`** (:118-120). This is an
   anti-escalation check, not robustness: the walker has no PS-bit handling, so
   confining it to PDPT[1] (all 4 KB pages) is what makes step 3 sound
   (vmspace.c:112-117). The bound is identical to the one `in_user_range`
   enforced — the walk is added *inside* it, not instead of it.
3. A full PML4→PDPT→PD→PT walk for **every page the span touches**, checking
   PRESENT *before* forming each child pointer — a non-present entry masks to
   phys 0, which the boot PD identity-maps, so dereferencing it would silently
   read garbage rather than fault (vmspace.c:126-143). The leaf must be
   PRESENT|USER, plus PG_RW when `need_write` (:144-152); the USER bit is
   enforced at the leaf only, since intermediates are always USER so the walker
   can reach user pages (vmspace.c:145-146).

**The funnel**: handlers never call it directly. `user_ok(uptr, len, write)`
resolves the *caller's* PML4 from its PCB, returning a safe EFAULT if
`virtual_address_space` is still NULL during the create→finalize window
(kernel/src/syscall.c:76-82). NUL-terminated strings go through
`copy_user_string`, which validates page-incrementally — each page is checked
before it is scanned, so a short string near a mapping edge is neither
over-read nor over-rejected (syscall.c:92-111). Coverage is total by audit: 32
direct `user_ok` spans plus 3 `copy_user_string` funnels (syscall.c:186, 584,
1216) — every user-pointer dereference in the 35-syscall table. Conventions at
the sites: clamp length *then* validate; copy-out validates the writable span
*before* consuming the side effect (IPC dequeue syscall.c:306, UDP RECVFROM
:880-889, QPU fetch/poll :1744/:1777); payloads bounce through kernel buffers
so the IF=1 net thread — which runs under its own CR3 — never sees a user
pointer (syscall.c:869-876).

**The same confinement at load time**: the ELF loader rejects any PT_LOAD
segment outside `[USER_VBASE, 0x80000000)`, any `p_vaddr+p_memsz` wrap, and
`p_filesz > p_memsz` (kernel/src/elf.c:89-99), on top of overflow-safe phdr
and file-extent bounds (elf.c:69-73, :86) — a below-`USER_VBASE` segment would
make `vmspace_map_page` corrupt arbitrary physical memory through the shared
2 MB frames. **Failure paths reclaim**: `spawn_elf_args` destroys the address
space on `elf_load` failure (syscall.c:2083-2091, PR #161) and
`finalize_user_process` does so on all three of its pre-bind early returns
(syscall.c:2001, :2011, :2036, PR #164); after the PCB bind, ownership
transfers and only `process_destroy` reclaims (syscall.c:2040-2044,
kernel/src/process.c:496). `vmspace_destroy` walks PDPT indices 1..511 only —
index 0 is the shared kernel half and is never freed (vmspace.c:170-176).

## Consequences

### Positive
- The #158 whole-OS DoS is closed: a hostile pointer costs its owner an EFAULT
  and nothing else. The proof is by attack — ghost-test hands SYS_SYSINFO an
  in-range unmapped pointer (0x40050000, the code↔stack gap) and a read-only
  code page for copy-out, expecting exactly −2 for both (user/ghost_test.c:250-265).
- The range check does double duty: DoS guard and escalation guard in one
  predicate, with the reasoning written at the check site (vmspace.c:112-117).
- Validate-before-consume means a late EFAULT can never strand state: an IPC
  message isn't dequeued, a QPU job isn't transitioned, a UDP datagram isn't
  received into a span that then turns out unwritable.

### Negative
- Pure software cost: a 4-level walk per touched page per validated span, on
  every syscall, with no TLB assist; `pml4[0]` is even re-checked per page
  (vmspace.c:130-132). Acceptable at ≤4 KB spans and one CPU; unmeasured beyond.
- Coverage is by convention, not construction: there is no `__user` type
  discipline or compiler check, so a future syscall can add an unguarded
  dereference — and any missed site is still the full ring-0 boot_panic, since
  faults remain fatal. The COPYGUARD gate attacks exactly one syscall
  (SYS_SYSINFO); the other 34 sites are covered by review, not by gate.
- The guard validates mappings, not meaning: struct contents arriving through
  a validated span are still attacker-shaped and every handler must re-validate
  fields (the twin-ABI request blocks of ADR-0001's syscall table).
- `copy_user_string` silently truncates over-length strings to `kmax-1`
  (syscall.c:109-110) — callers cannot distinguish truncation from exact fit.
- Known micro-leak: `vmspace_create` leaks the PML4 frame if the PDPT
  allocation fails (vmspace.c:49-52).

### Residual risks
- The no-PS-pages invariant is load-bearing and implicit: mapping a huge page
  into the user half, or widening the user half past PDPT[1], without teaching
  the walker PS bits reopens the false-PASS escalation. Nothing asserts the
  invariant at map time; `vmspace_map_page` simply cannot produce PS pages today.
- Validate-then-copy is non-atomic. It is sound only because syscalls run
  cli'd on one CPU and no other context mutates a live process's user mappings
  mid-syscall. SMP, demand paging, or an IF=1 unmap path would each break the
  guarantee and require revisiting every site.
- `vmspace_destroy` must never run on the CR3-loaded space
  (vmspace.h:53-57); the discipline is a call-site convention enforced by
  `process_destroy`'s refuse-if-current check, not by the function itself.

## Evidence

- Shipped in: PR #160 — `vmspace_user_ok` + conversion of every copy in/out,
  closing #158 (commit 6da9d9b).
- Shipped in: PR #161 — ELF loader bounds + user-half segment confinement, and
  `vmspace_destroy` on the `elf_load` failure path (commit 0b53805).
- Shipped in: PR #164 — resource-leak sweep completing the
  vmspace_destroy-on-error discipline across `finalize_user_process` and the
  flat-blob spawn path (commit 89053d6).
- Key code: kernel/src/vmspace.c:105-155 (`vmspace_user_ok`);
  kernel/src/syscall.c:76-82 (`user_ok`), :92-111 (`copy_user_string`);
  kernel/src/elf.c:89-99 (segment confinement); kernel/src/syscall.c:1989-2094
  (spawn failure-path reclaim); kernel/src/vmspace.c:163-194 (`vmspace_destroy`).
- CI gates: COPYGUARD — two required boot-log lines, "unmapped pointer denied"
  and "RO copy-out denied" (Makefile:611, :618), emitted by the ring-3 probes
  at user/ghost_test.c:252-265. Anti-vacuous by construction: without the fix
  the probe faults in ring 0 and the boot halts before *any* later gate line
  can print (Makefile:605-610). ELFGUARD — malformed spawns rejected with zero
  frame leak (kernel/src/main.c:348-351); SPAWNLEAK — failed spawn reclaims its
  address space (main.c:358-361). All three panic the boot on failure.
