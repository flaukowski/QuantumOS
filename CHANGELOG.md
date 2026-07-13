# Changelog

All notable changes to QuantumOS. The project uses semantic-ish milestone versions;
`v0.4.0` is the first *tagged* release — earlier versions are retrospective milestone
groupings of the 126 merged PRs (#7–#183), recorded here so the history reads honestly.
Full per-PR provenance: `git log` and the PR list on GitHub.

## [Unreleased]

### ADR-0020 v1 contract freeze — pre-freeze corrections (guest syscall ABI)

Groundwork before the golden-diff ABI freeze gate lands, so v1 pins a *correct*
surface (`user/usys.h`):
- Corrected the `field_info_` errno docstring: it cited Linux `-14 EFAULT` / `-22
  EINVAL`; the syscall actually returns QuantumOS `SYSCALL_EFAULT` (-2) /
  `SYSCALL_EINVAL` (-1).
- Added 5 symmetric user-side twin-ABI `_Static_assert`s (`udp_req_t`=24,
  `field_imprint_req_t`=76, `field_recall_req_t`=76, `field_recall_out_t`=136,
  `user_args_t`=516) so a struct-size drift fails BOTH ring builds, not just the
  kernel's.
- Annotated the intentional errno-band overlaps as frozen-v1 decisions:
  `svc_restarts()` returns `-1` (not-a-service) sharing `SYSCALL_EINVAL`, and
  `qseed_value()` returns a raw u64 not partitioned from the negative-errno band.
- Split the 6 kernel-side ring-crossing `_k_t` twins (`udp_req_k_t`,
  `cap_derive_req_k_t`, the four `qpu_*_k_t`) out of `kernel/src/syscall.c` into a
  new kernel-internal header `kernel/include/kernel/syscall_abi.h`, so the
  upcoming ABI freeze probe can `#include` and compiler-measure them. ABI-neutral
  (identical structs, `_Static_assert`s moved with them).

### Added — v1 golden ABI freeze gate (ADR-0020)

`contracts/abi/v1.golden` freezes the guest+kernel ABI contract — the 35 syscall
numbers, the sub-op namespaces, the errno table, capability permission bits, and
the ring-crossing struct sizes — and CI diffs it on every push/PR. The values are
**compiler-measured**, not parsed from source: two probe TUs (`user/abi_probe.c`,
`kernel/src/abi_probe_kern.c`) emit the contract into a `.abi_ents` section under
the real build flags, and `scripts/extract-abi.py` reads it back with `objcopy`.
The gate cross-checks that the user/kernel twins agree, diffs against the
committed golden (an intended change is a visible golden diff, never silently
regenerated — `make regen-abi-golden` is human-only), and a teeth-check compiles
a mutated `SYS_QPU` and asserts the gate reddens, so a broken generator can't ship
vacuously green. New CI job `abi-golden` (gcc + binutils, no boot, no pip) that
also gates `release`.

The frozen surface now also pins ring-crossing struct **field offsets**
(`__builtin_offsetof` for every field of the six `udp`/`cap_derive`/`qpu_*`
twins, cross-checked user↔kernel) — so a size-preserving field reorder, which
the size check alone would miss, reddens the gate. The capability resource-type
namespace (`cap_resource_type_t`), the arg-page field offsets, and the device-ID
namespace (`net`/`qpu`/`com2`/`console`/`disk` — the `resource_id` a
`CAP_RESOURCE_DEVICE` capability names) are frozen too (237 golden entries). With
this, **ADR-0020 is Accepted** for the guest syscall ABI (contract a) — frozen and
CI-enforced. The MCP-schema lane (contract b), the COM2/attestation lane
(contract c, now unblocked by ADR-0019), and the durable audit-format freeze
remain scoped as deferred follow-ups.

## [0.5.0] — 2026-07-11 — Agent-reachable QPU, hardening, dead-code payoff

Post-v0.4.0 increments landed autonomously through the panel → gate (revert-and-confirm)
→ merge pipeline. PRs #187–#190.

### Added
- **`qos_qpu_submit` — the 22nd MCP tool** (#189): an agent can now run a named quantum
  circuit (`bell` / `ghz` / `grover`) on the in-OS QPU broker and get the exact integer
  result back — capability-gated (only `swarm_svc` holds the submit cap), quota-bounded
  (`qsub_max`), and recorded in the authority ledger (`AUDIT_QSUBMIT`). The
  conscience-before-wallet demo made agent-facing. Gated by `ci-smoke-qsubmit`.
- **Published to PyPI**: `pip install quantumos-host-tools` is live; tagged releases now
  auto-publish (the `PYPI_API_TOKEN` secret is wired into `release.yml`).

### Fixed
- **PMM heap-reservation gap** (#187): the kernel heap's backing frames are now reserved
  in the physical allocator, so the low-first rover can never hand a live-heap frame to a
  page table or ELF segment (an arbitrary-corruption path reachable only under a large
  residency storm). New `PMMHEAP` boot self-test, revert-confirmed.
- **Multiboot2-magic trap** (#187): the kernel is Multiboot1-only, so an MB2 magic is now
  refused with an honest panic instead of parsing a v2 info block as v1 (which would lose
  the cmdline and wild-write the framebuffer). New `MB2REJECT` self-test.
- **`qos_memory_import` docstring** (#190): corrected — each memory's Kannaka similarity
  *is* carried into its slot energy (the docstring had claimed it was not).
- **Society error identity** (#190): the society tools no longer misattribute a failure to
  the unrelated single-VM identity (`_society_err`).

### Changed
- **Dead-code payoff** (#188): deleted the unwired `cap_transfer` helper, the
  declared-never-defined `apic_*` prototypes, and five stale duplicate bare headers under
  `kernel/include/` — −641 lines, proven dead by a green `ci-smoke`.

## [0.4.0] — 2026-07-11 — Quantum stack, agent societies, hardened (first tagged release)

The agent-native arc completed and battle-tested. PRs #151–#183.

### Added
- **Quantum stack** (epics #148/#149/#150, PRs #151–#155): `qsv`, an *exact* Gaussian-integer
  quantum state-vector citizen (zero rounding, digest-vs-host-mirror CI gate); a capability-gated
  kernel **QPU job broker** (`SYS_QPU`, opaque circuits only, per-manifest `qsub` quota); a COM2
  quantum-submit transport over the attested bridge; a host gateway dispatching
  **PennyLane lightning.qubit / lightning.gpu (cuQuantum) / CUDA-Q / qBraid real QPUs**, all
  cross-oracled against the exact engine (≤1e-9).
- **Agent society arc** (PRs #171–#179): the end-to-end agent-native showcase (`agentdemo` boot
  token) — an orchestrator citizen delegates narrowed field capabilities to sub-agents it spawns
  itself over **spawn-time parent↔child IPC channels**; content-consensus digests verified per
  kernel-vouched sender; **division-of-labor** private field workspaces; a **society of
  societies** — two kernels exchanging verified society results over the field-sync wire; the
  ring-3 roster refactored to `kernel/src/citizens.c`.
- **CI gates**: qsh singleton-IPC-cap invariant (#176), disk *upgrade* path — frozen audit-ledger
  home survives field growth (#183), qseed-handoff smoke incl. the errno-collision gate (#168).

### Fixed
- **Adversarial bug-hunt campaign** (PRs #156–#167): ~31 real bugs across quantum, networking,
  trust-core, process/memory, and lifecycle code — 0 false positives across 8 hunts. Highlights:
  closed a ring-3 → whole-OS DoS in syscall copy-in/out (#158→#160, `COPYGUARD` gate); DNS answers
  bound to their query (on-link poisoning, #162); spawn error paths reclaim address spaces (#164);
  argv buffer cleared between spawns (#165).

### Changed
- Performance: amortized-O(1) frame allocator via a search rover (#169); capability read-lookups
  bounded by a high-water mark (#170).
- Dead code deleted per the unwired-optionality rule: the never-wired shared-memory region
  subsystem (#180) and the orphaned duplicate `ipc.h` (#181).

## [0.3.0] — 2026-07-10 — Field memory, agent surface, societies *(retrospective, untagged)*

The OS becomes agent-native. PRs #112–#147.

### Added
- **Associative memory as a syscall**: `SYS_IMPRINT`/`SYS_RECALL`/`SYS_FIELD_INFO` — capability-
  scoped kernel field regions with Q15 wave-resonance scoring; recall from ~15%-corrupted cues
  (epic #95). **Memories survive reboot** on the checksummed QDSK volume (epic #96); sync refuses
  foreign disks (#115).
- **Two kernels, one field** (epic #97): `fieldsyncd` couples ghostd oscillator fields over UDP
  (R_x 0.03→0.99); then the **N-way mean-field society** (epic #139) — N kernels with distinct
  qseeds synchronize over a multicast L2, min-pairwise verdict.
- **TCP server + httpd** (epic #98): QuantumOS serves a live status page.
- **MCP server** (epic #99) and the deepened **agent toolbox** (epic #125): 21 tools — boot,
  qsh scripting, field imprint/recall, fs, fetch, qrand, societies — every result carrying the
  Lamport-verified boot identity. **Kannaka HRM bridge** (epic #127): agent memories flow between
  the kernel field and the host memory system.
- **QuantumOS in the browser** (epic #100): the real kernel under qemu-wasm on GitHub Pages.
- **The trust core** (Phase D, epics #133/#135/#137/#144): the capability **authority ledger**
  (every grant/deny/spawn/revocation recorded, durable across reboots #146, REVOKE-vs-REAP
  attribution #147); **explicit intent manifests** with enforced quotas (spawn #136, CPU #145,
  quantum submissions #152); **cross-ring one-hop capability delegation** (`SYS_CAP_DERIVE` #138)
  with cascade revocation on delegator death.

## [0.2.0] — 2026-07-08 — A real microkernel OS *(retrospective, untagged)*

Non-booting skeleton → an interactive OS on real hardware, in six days. PRs #27–#111.

### Added
- **Core microkernel in one day** (#31–#46): multiboot v1 + long-mode boot, 100 Hz PIT preemptive
  round-robin scheduler, capability-based security, ring-3 user mode with fault containment,
  per-process address spaces, in-kernel ELF64 loader, services-as-ring-3-processes with
  capability-checked IPC, watchdog heartbeats, real kernel heap.
- **ghostOS/HRM-0 citizens** (#53–#59): `ghostd` Hopfield–Kuramoto associative memory;
  `SYS_QRAND` with honest qseed provenance; `paradoxd` field-gated paradox resolver; COM2 swarm
  bridge with **Lamport-attested boot**; the resonant scheduler wired and *measured honestly*
  (it loses to round-robin — verdict published in CI).
- **Interactive OS** (epic #62): `qsh` shell, embedded ustar initrd + VFS, `SYS_SPAWN`/`SYS_WAITPID`
  with argv — boot → shell → exec → exit, all CI-gated.
- **Persistent storage** (epic #71): ATA PIO driver + writable ramfs overlay + `sync`; two-boot
  survival proven in CI.
- **Networking** (epics #73/#80/#82): PCI/RTL8139, ARP, IPv4/UDP/DHCP, ICMP, DNS, `SYS_RESOLVE`,
  ring-3 UDP sockets, TCP client — the shell fetches a web page. CMOS RTC (`date`).
- **libq + native citizens** (#86–#92): ring-3 runtime (arena heap, integer printf, fixed-point
  math) and native apps — `consciousnessd`, `kannakad`, `quantumd`, `qtop`.
- **Real hardware** (epic #101): VGA screen console, GRUB ISO (USB-flashable), quiet boot; a real
  laptop boots to an interactive shell on its own screen and keyboard.

## [0.1.0] — 2026-02-13 — Bootstrap foundation *(retrospective, untagged)*

PRs #7–#22: the original skeleton — architecture documents, message-passing IPC, process
management, CI scaffolding, the resonant-scheduler concept, and a kernel that compiled but did
not yet boot. Dormant until the 2026-07-02 revival.
