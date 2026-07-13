# 21. Honest Memory to the 1 GB Identity Window

Date: 2026-07-11
Status: Accepted (2026-07-13; implemented PR-1..PR-4)

> **Update (2026-07-11).** The two confirmed latent **defects** below shipped as a focused
> hardening increment — the PMM now reserves the kernel heap's backing frames
> (`[__heap_start, __heap_end)`) and rounds the bitmap size up, and `boot_validate_multiboot`
> refuses the Multiboot2 magic (the kernel is Multiboot1-only). Both are gated: the boot
> self-tests `pmm_heap_reservation_selftest` / `boot_validate_selftest` panic the boot before
> `QuantumOS ready` if reverted, surfaced as the `PMMHEAP:` and `MB2REJECT:` markers in
> `make ci-smoke` and the integration `SHELL_GATES`.
>
> **Update (2026-07-13) — Accepted, feature complete.** The dynamic sizing landed across four
> revert-confirmed increments: PR-1 the 1 GB clamp + static-layout floor + guards (#213), PR-2
> the synthetic clamp teeth-check `PMMCLAMP` (#214), PR-3 the untrusted-mmap parser
> `multiboot_parse_memory` that replaces the `128 MB` hardcode and sizes the PMM from real RAM,
> emitting the greppable `PMMSIZE=<hex>` marker (#215), and PR-4 (this increment) the
> high-memory reachability proof `PMMHIGH` (allocate a top-of-pool frame ≥ 128 MB and prove the
> boot.S 1 GB identity map reaches it via sentinel writeback), the residency-storm proof
> `PMMSTORM`, and the differential `-m 256M`/`-m 512M` CI legs. Every "un-fakeable gate" named in
> the design below is now live: sizing tracks `-m` (0x7FE0 / 0xFFE0 / 0x1FFE0 at 128/256/512 MB —
> a hardcode gives 0x8000 at all three and reddens the 256M leg), a high frame is written and
> read back through the identity map, and no heap-range frame survives a 1024-frame drain. The
> 1 GB clamp and the >1 GB out-of-scope boundary stand as written. The differential legs use
> `-m 256M`/`-m 512M` rather than the design's tentative `-m 1G` — the same proof (real high RAM
> handed + written back) at a size QEMU boots well within the gate timeout.

## Context

The physical memory manager hardcodes 128 MB (`// 128MB for testing`,
kernel/src/memory.c:568-569); RAM beyond that is silently ignored on real hardware (a
documented README limit, ADR-0017). Two latent defects found during next-phase
planning make this more than a feature request — they are confirmed real (verified by
a 3-lens skeptical pass each) but not reachable in the shipped boot/CI/demo:

1. **PMM/kheap reservation gap (Medium).** `pmm_init` reserves only frames 0..655
   (kernel image + bitmap), but `kheap_init` backs the 64 MB kernel heap directly on
   link.ld's `ram` region at frames 4354..20737 — which are left **free** in the PMM
   bitmap (memory.c:63-74, 414-440; no other reservation exists). The low-first
   compacting allocator can therefore hand out a live-heap frame and the subsequent
   `memset` zeroes a live heap block header → shredded free-list → wild kernel
   writes. Reachable only via a deliberate ~217-concurrent-process residency storm
   through qsh's unbounded spawn (the "spawn churn marches the rover" theory was
   *refuted* — the rover rewinds on free). No gate guards the boundary.
2. **Multiboot2-magic trap (Low, dead today).** `boot_validate_multiboot` accepts the
   MB2 magic (0x36d76289) while every consumer parses MB1 fixed offsets — under an MB2
   info block, all cmdline tokens are lost and `fb_init_from_multiboot` does a wild
   framebuffer write (main.c:706-719, fb.c:106-128). Dead code today because `boot.S`
   carries only an MB1 header; it arms the day a UEFI/Limine MB2 header is added.

## Decision (proposed)

Size the PMM dynamically from the multiboot memory map, **honestly clamped at 1 GB**,
and fix the two latent defects in the same phase.

- **Dynamic sizing, hard 1 GB clamp.** Parse the bootloader-supplied memory map
  (untrusted input — bounds-check it so a hostile/garbage map cannot size a wild
  bitmap) and size the PMM bitmap to real RAM, rounding `bitmap_size` **up** (the
  current `/8` truncates; latent-only at 128 MB where 32768/8 = 4096 exactly). Clamp
  at 1 GB: boot.S identity-maps exactly the first 1 GB, and the kernel writes user
  frames through that supervisor identity VA, so a frame above 1 GB is unmappable —
  handing one out would be a #158-class escalation. No boot.S change is needed;
  frames 128 MB–1 GB are already identity-mapped.
- **Reserve the heap.** Reserve frames covering `[__heap_start, __heap_end)` in
  `pmm_init`, closing defect (1).
- **Reject the MB2 magic** in `boot_validate_multiboot` until a real MB2 tag parser
  exists, closing defect (2) — turning a future silent corruption into a clean panic.
- **Explicitly out of scope:** >1 GB RAM / a higher-half physmap. That is a separate
  future epic — it would invalidate `vmspace_user_ok`'s anti-escalation confinement
  (ADR-0003), the ELF segment bounds, and every VA==PA assumption (PMM pointer math,
  RTL8139 DMA, argv fills). Naming it here keeps it from creeping into this phase.

## Consequences

### Positive
- Real hardware with >128 MB uses its RAM up to 1 GB, and the 1 GB honesty is
  enforced, not assumed — every handed frame is provably mappable.
- Two confirmed latent kernel-corruption paths close with small, local changes in the
  file the phase already touches.
- Sets up the boot-info/mmap parser seam that the deferred UEFI phase (ADR-0017
  cross-ref) would otherwise re-implement.

### Negative
- The memory map is untrusted bootloader input — a new parsing surface that must be
  bounds-checked, the one real risk this phase adds.
- The 1 GB clamp is a real ceiling: the honest fix is *not* "support all RAM", and the
  README limit changes from "128 MB" to "1 GB", not "unbounded".
- The reservation shrinks usable frames by the heap's 16 384 frames — correct, but it
  makes the residency-storm DoS ceiling slightly lower (fewer free frames), which is
  the honest trade for not corrupting the heap.

### Residual risks
- The residency-storm DoS (256-slot table-full) still exists as a clean
  denial-of-service after the reservation fix — this phase removes the *corruption*,
  not the DoS; bounding concurrent residency is a separate concern.
- QEMU `-m 1G` on the CI runner must be confirmed to boot within gate timeouts; every
  existing gate stays at `-m 128M` so a regression cannot hide in the new leg.

## Evidence (baseline + verified defects)
- Hardcode: kernel/src/memory.c:568-569; identity map: kernel/src/boot.S:69-87;
  user half via PDPT[1]: kernel/src/vmspace.c
- Defect 1 verified: pmm_init reserves 0..655 (memory.c:63-74); kheap on link.ld ram
  frames 4354..20737 unreserved (memory.c:414-440, link.ld); rover rewinds on free
  (memory.c:146-148) — so the trigger is a residency storm, not churn
- Defect 2 verified: main.c:706-719 (MB2 magic accepted), fb.c:106-128 (offset-88
  deref+write), boot.S MB1-only header (dead code today)
- Un-fakeable gates (design): a `-m 512M` leg that sizes real RAM AND hands +
  writes-back a frame ≥ 0x8000000 while asserting **every** handed frame < 0x40000000;
  a `-m 128M` anti-regression leg; revert `memory.c:569` → the >128 MB leg reddens;
  a residency-storm leg proving no heap-range frame is ever handed out
- Cross-references: ADR-0003 (user-pointer confinement the >1 GB epic would break),
  ADR-0005 (single-core), ADR-0017 (honest limits, the MB2 debt)
