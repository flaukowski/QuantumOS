/**
 * QuantumOS Intent Manifest (epic #135 — Phase D increment 2)
 *
 * A per-pid, kernel-held record of DECLARED intent: the allow-set of
 * {resource_type, resource_id} a citizen was registered to touch, plus the
 * first enforced quota (spawn_max) and live CPU-tick accounting. The manifest
 * is built from the SAME grant flags that mint a service's capabilities and is
 * bound inside the same interrupt-off window, so intent and authority are
 * never half-committed. Checked at the capability-gated syscall sites AFTER
 * cap_find succeeds: a capability that exceeds declared intent is refused
 * (AUDIT_MDENY) — the outer bound that stays meaningful once SYS_CAP_DERIVE
 * delegation makes capabilities transferable.
 *
 * HONEST SCOPE (v1): for shipped citizens caps are minted from the same
 * grants the manifest records, so the intent check refuses nothing today —
 * its value is (a) intent made explicit and inspectable (SYS_MANIFEST),
 * (b) the substrate delegation will be bounded by, and (c) the quota carrier.
 * The SPAWN QUOTA is the non-vacuous enforcement now: quota checks precede
 * side effects, successful spawns charge, violations land in the authority
 * ledger as AUDIT_QUOTA. IPC is deliberately OUTSIDE the manifest: IPC caps
 * are pair-wise runtime wiring (minted per peer at boot), not declarative
 * intent — the audit ledger's documented IPC gap applies here too. Matching
 * is (resource_type, resource_id) membership ONLY; permission bits are
 * recorded for inspection but enforced by the capability layer alone, so the
 * two layers can never disagree about perms semantics.
 *
 * UNBOUND = ALLOW: a pid with no bound manifest passes every intent check.
 * Every ring-3 process is bound at creation (finalize_user_process binds the
 * restrictive default; start_slot overwrites it with the grant-derived
 * manifest), so the allow-by-default population is exactly ring 0, which
 * never traverses int 0x80. Keep it that way: any future spawn path that
 * bypasses finalize_user_process silently reopens the hole.
 *
 * CONCURRENCY: bind/clear/charge run from syscall context AND the IF=1
 * service health monitor (watchdog rebirth re-binds from that thread), so
 * every mutator is self-contained under a local irqsave — the audit.c rule,
 * NOT field.c's cli'd-caller assumption. manifest_tick runs inside the timer
 * interrupt itself (IF already 0). Single CPU throughout.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_MANIFEST_H
#define KERNEL_MANIFEST_H

#include <kernel/types.h>

#define MANIFEST_MAX_ENTRIES 8 /* intent rows per process (7 grant flags exist) */

/* One-shot text dump cap (SYS_MANIFEST). Sized for ~27 fully-loaded bound
 * manifests — >2x a realistic boot's bound population (~15). BOUND pids only
 * are emitted, LINE-ATOMICALLY (a row is emitted whole or not at all — a
 * mid-line cut of "perms=0x1f" to "perms=0x1" would still parse host-side,
 * with a WRONG value); when clipped, the reserved "MANIFEST: truncated=1"
 * tail line says so instead of lying by omission. */
#define MANIFEST_TEXT_MAX 16384
#define MANIFEST_LINE_MAX 96

typedef struct {
    uint32_t resource_type; /* cap_resource_type_t */
    uint32_t resource_id;
    uint32_t permissions; /* recorded for inspection; NOT matched (cap layer enforces) */
} manifest_entry_t;

typedef struct {
    uint8_t bound; /* 0 = no manifest (ring 0 / cleared slot) */
    uint8_t entry_count;
    uint16_t reserved;
    uint32_t spawn_max;  /* successful-spawn quota; 0 = bound but unlimited */
    uint32_t spawn_used; /* successful spawns this incarnation (rebirth resets) */
    uint32_t cpu_limit;  /* max cumulative scheduled-in ticks; 0 = unlimited (epic #144) */
    uint64_t cpu_ticks;  /* timer ticks scheduled-in; ENFORCED against cpu_limit (#144) */
    manifest_entry_t entries[MANIFEST_MAX_ENTRIES];
} manifest_t;

/* Bind pid's manifest — an UNCONDITIONAL last-write-wins copy. Rebinding a
 * bound slot is the normal service path: finalize_user_process binds the
 * restrictive default for every ring-3 process, then start_slot overwrites
 * it with the grant-derived manifest inside the same cli window that mints
 * the caps. Self-contained irqsave; safe from the health-monitor thread. */
void manifest_bind(uint32_t pid, const manifest_t *m);

/* Clear pid's manifest (whole-entry memset, so a recycled pid can never
 * inherit its predecessor's intent, counters, or cpu_ticks). Called from
 * process_destroy beside cap_revoke_all_for_process. */
void manifest_clear(uint32_t pid);

/* Append ONE allow row to an already-bound manifest — the runtime intent
 * extension SYS_CAP_DERIVE performs when it delegates a capability (epic #137):
 * a delegated cap is inert unless the recipient's manifest also allows the
 * resource. Self-contained irqsave (the same discipline manifest_bind uses;
 * the health monitor rebinds from IF=1). Returns true if the row is present
 * after the call: false if the pid is unbound (caller must pre-check) or the
 * manifest is full (entry_count >= MANIFEST_MAX_ENTRIES); an existing
 * (resource_type, resource_id) row is an idempotent no-op success. Perms are
 * recorded for inspection, NOT matched by manifest_check. */
bool manifest_grant(uint32_t pid, uint32_t resource_type, uint32_t resource_id,
                    uint32_t permissions);

/* True if pid has a BOUND manifest with a free entry slot. SYS_CAP_DERIVE
 * pre-checks this BEFORE minting the child cap, so manifest_grant cannot fail
 * after a capability already exists (no undo path). */
bool manifest_has_room(uint32_t pid);

/* True if pid has a BOUND manifest with a nonzero cpu_limit it has EXCEEDED
 * (cpu_ticks >= cpu_limit) — the CPU-quota enforcement predicate (epic #144).
 * Plain read, no lock: safe because every writer (manifest_bind/clear) runs
 * cli'd and manifest_tick is a single aligned-u64 increment on one CPU; the
 * intended caller is scheduler_tick (timer IRQ, IF=0). cpu_limit==0 (every
 * shipped citizen) is unlimited, so this is constant-false for all but a
 * citizen that DECLARES a finite budget. */
bool manifest_over_budget(uint32_t pid);

/* Intent check for a capability-gated syscall site. Call AFTER cap_find
 * succeeds. Returns 1 (allowed: unbound pid, or a bound manifest with a
 * matching {resource_type, resource_id} row) or 0 (bound manifest without a
 * matching row) — a 0 has already recorded AUDIT_MDENY; the caller returns
 * EPERM. */
int manifest_check(uint32_t pid, uint32_t resource_type, uint32_t resource_id,
                   uint32_t permissions);

/* Spawn-quota precheck: MUST run before any spawn side effect (copy-in,
 * parse, initrd lookup). Returns 1 (under quota / unbound / unlimited) or 0
 * (spawn_used >= spawn_max) — a 0 has already recorded AUDIT_QUOTA. */
int manifest_spawn_precheck(uint32_t pid);

/* Charge one successful spawn. Call ONLY after spawn_elf_args succeeds —
 * failed attempts (ENOENT/EINVAL/EIO) never consume quota. */
void manifest_spawn_charge(uint32_t pid);

/* CPU accounting: one timer tick consumed by pid. Called from scheduler_tick
 * (IF already 0) BEFORE the quantum early-return, so it counts ticks, not
 * reschedules. Unbound pids (ring 0, idle) hit a dead entry — invisible in
 * the bound-only dump. */
void manifest_tick(uint32_t pid);

/* Format every BOUND manifest as "MANIFEST: ..." text lines into buf
 * (line-atomic, bounded, NUL-terminated; appends "MANIFEST: truncated=1"
 * when rows were dropped). Returns bytes written (excluding the NUL). */
size_t manifest_format(char *buf, size_t max);

/* Boot self-test: proves the deny path is LIVE (on the shipped system caps
 * and manifests are minted from the same grants, so no citizen ever trips
 * MDENY — without this a constant-true manifest_check ships green). Binds a
 * synthetic manifest, drives the real check helper to a deny (recording a
 * real AUDIT_MDENY) and a quota refusal (recording a real AUDIT_QUOTA),
 * verifies allow/unbound/clear semantics, and logs
 * "manifest self-test: PASS (MDENY recorded)". Returns 0 on success. */
int manifest_selftest(void);

#endif /* KERNEL_MANIFEST_H */
