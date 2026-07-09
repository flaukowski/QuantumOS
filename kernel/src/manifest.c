/**
 * QuantumOS Intent Manifest implementation (epic #135 — Phase D increment 2).
 * See kernel/include/kernel/manifest.h for the model, scope, and concurrency
 * rule.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/manifest.h>
#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/process.h> /* MAX_PROCESSES */
#include <kernel/syscall.h> /* SPAWN_RESOURCE_ID */
#include <kernel/boot.h>

/* Keyed by pid (pids are process-table slot indices < MAX_PROCESSES). Lives
 * in .bss: a zeroed slot is unbound, which is the correct initial state. */
static manifest_t manifest_table[MAX_PROCESSES];

/* Self-contained IRQ save/restore (the audit.c rule): mutators are called
 * from syscall context AND the IF=1 service health monitor thread. */
static inline uint64_t man_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void man_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static void man_memset(void *dst, uint8_t v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = v;
    }
}

void manifest_bind(uint32_t pid, const manifest_t *m) {
    if (pid >= MAX_PROCESSES || !m) {
        return;
    }
    uint64_t flags = man_irq_save();
    /* Unconditional last-write-wins copy: finalize_user_process binds the
     * restrictive default for every ring-3 process; start_slot then
     * overwrites it with the grant-derived manifest inside the same cli
     * window. A bind-once "hardening" here would leave a watchdog-reborn
     * qsh holding the child manifest — console MDENY, shell suicide. */
    manifest_table[pid] = *m;
    manifest_table[pid].bound = 1;
    if (manifest_table[pid].entry_count > MANIFEST_MAX_ENTRIES) {
        manifest_table[pid].entry_count = MANIFEST_MAX_ENTRIES;
    }
    man_irq_restore(flags);
}

void manifest_clear(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return;
    }
    uint64_t flags = man_irq_save();
    /* Whole-entry memset: a recycled pid must never inherit its
     * predecessor's intent rows, spawn counters, or cpu_ticks. */
    man_memset(&manifest_table[pid], 0, sizeof(manifest_table[pid]));
    man_irq_restore(flags);
}

int manifest_check(uint32_t pid, uint32_t resource_type, uint32_t resource_id,
                   uint32_t permissions) {
    if (pid >= MAX_PROCESSES) {
        return 1; /* not a table pid — cap layer already vouched for it */
    }
    const manifest_t *m = &manifest_table[pid];
    if (!m->bound) {
        return 1; /* unbound = allow (ring 0 only; see manifest.h) */
    }
    /* Membership match on (resource_type, resource_id) ONLY: permission
     * semantics stay the capability layer's job, so the two layers cannot
     * diverge (a manifest row narrower than the minted cap would brick the
     * console on the first prompt write). */
    for (uint8_t i = 0; i < m->entry_count; i++) {
        if (m->entries[i].resource_type == resource_type &&
            m->entries[i].resource_id == resource_id) {
            return 1;
        }
    }
    /* A held capability exceeded declared intent — provable, not silent. */
    audit_record(AUDIT_MDENY, pid, AUDIT_V_EPERM, resource_type, resource_id, permissions);
    return 0;
}

int manifest_spawn_precheck(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return 1;
    }
    uint64_t flags = man_irq_save();
    const manifest_t *m = &manifest_table[pid];
    int ok = !m->bound || m->spawn_max == 0 || m->spawn_used < m->spawn_max;
    man_irq_restore(flags);
    if (!ok) {
        audit_record(AUDIT_QUOTA, pid, AUDIT_V_EPERM, CAP_RESOURCE_PROCESS, SPAWN_RESOURCE_ID,
                     CAP_EXECUTE);
    }
    return ok;
}

void manifest_spawn_charge(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return;
    }
    uint64_t flags = man_irq_save();
    if (manifest_table[pid].bound) {
        manifest_table[pid].spawn_used++;
    }
    man_irq_restore(flags);
}

void manifest_tick(uint32_t pid) {
    /* Timer-interrupt context: IF is already 0, a plain aligned-u64
     * increment cannot tear against readers (every reader runs cli'd or
     * under irqsave on this single CPU). Unbound entries take the bump
     * invisibly — the dump emits bound pids only. */
    if (pid < MAX_PROCESSES) {
        manifest_table[pid].cpu_ticks++;
    }
}

/* ---- freestanding formatting (no libc; the audit.c idiom) ---- */
static size_t put_str(char *b, size_t o, size_t max, const char *s) {
    while (*s && o < max - 1) {
        b[o++] = *s++;
    }
    return o;
}

static size_t put_u64(char *b, size_t o, size_t max, uint64_t v) {
    char t[20];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v > 0 && n < 20) {
        t[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && o < max - 1) {
        b[o++] = t[--n];
    }
    return o;
}

static size_t put_hex8(char *b, size_t o, size_t max, uint32_t v) {
    if (o < max - 1) {
        b[o++] = '0';
    }
    if (o < max - 1) {
        b[o++] = 'x';
    }
    char t[8];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v > 0 && n < 8) {
        uint32_t d = v & 0xF;
        t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    while (n > 0 && o < max - 1) {
        b[o++] = t[--n];
    }
    return o;
}

static const char *type_name(uint32_t t) {
    /* Mirrors cap_resource_type_t (capability.h; the audit.c table). */
    static const char *const names[] = {"MEM", "IPC", "DEV", "QRNG", "PROC", "SVC", "FIELD"};
    return t < (sizeof(names) / sizeof(names[0])) ? names[t] : "?";
}

/* Emit one whole line into buf, or nothing at all (line-atomic): a clipped
 * "perms=0x1f" would still parse host-side as perms=0x1 — wrong data beats
 * no data only when you can tell them apart, and you can't. */
static size_t put_line(char *buf, size_t o, size_t budget, const char *line, size_t linelen) {
    if (o + linelen >= budget) {
        return (size_t)-1; /* would clip — emit nothing */
    }
    for (size_t i = 0; i < linelen; i++) {
        buf[o + i] = line[i];
    }
    return o + linelen;
}

size_t manifest_format(char *buf, size_t max) {
    static const char tail[] = "MANIFEST: truncated=1\r\n";
    const size_t tail_len = sizeof(tail) - 1;
    if (!buf || max < tail_len + 1) {
        return 0;
    }
    /* Reserve the tail's bytes up front so "truncated=1" itself always fits. */
    size_t budget = max - tail_len - 1;
    size_t o = 0;
    int truncated = 0;

    uint64_t flags = man_irq_save();
    for (uint32_t pid = 0; pid < MAX_PROCESSES && !truncated; pid++) {
        const manifest_t *m = &manifest_table[pid];
        if (!m->bound) {
            continue;
        }
        char line[MANIFEST_LINE_MAX];
        size_t l = 0;
        l = put_str(line, l, sizeof(line), "MANIFEST: pid=");
        l = put_u64(line, l, sizeof(line), pid);
        l = put_str(line, l, sizeof(line), " bound=1 spawn=");
        l = put_u64(line, l, sizeof(line), m->spawn_used);
        l = put_str(line, l, sizeof(line), "/");
        if (m->spawn_max == 0) {
            l = put_str(line, l, sizeof(line), "*");
        } else {
            l = put_u64(line, l, sizeof(line), m->spawn_max);
        }
        l = put_str(line, l, sizeof(line), " cpu_ticks=");
        l = put_u64(line, l, sizeof(line), m->cpu_ticks);
        l = put_str(line, l, sizeof(line), "\r\n");
        size_t r = put_line(buf, o, budget, line, l);
        if (r == (size_t)-1) {
            truncated = 1;
            break;
        }
        o = r;

        for (uint8_t i = 0; i < m->entry_count; i++) {
            l = 0;
            l = put_str(line, l, sizeof(line), "MANIFEST: pid=");
            l = put_u64(line, l, sizeof(line), pid);
            l = put_str(line, l, sizeof(line), " allow res=");
            l = put_str(line, l, sizeof(line), type_name(m->entries[i].resource_type));
            l = put_str(line, l, sizeof(line), ":");
            l = put_u64(line, l, sizeof(line), m->entries[i].resource_id);
            l = put_str(line, l, sizeof(line), " perms=");
            l = put_hex8(line, l, sizeof(line), m->entries[i].permissions);
            l = put_str(line, l, sizeof(line), "\r\n");
            r = put_line(buf, o, budget, line, l);
            if (r == (size_t)-1) {
                truncated = 1;
                break;
            }
            o = r;
        }
    }
    man_irq_restore(flags);

    if (truncated) {
        o = put_str(buf, o, max, tail);
    }
    buf[o < max ? o : max - 1] = '\0';
    return o;
}

/* ---- boot self-test ---- */
#define MST_ASSERT(cond, msg)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            boot_log("manifest self-test FAILED: ");                                               \
            boot_log(msg);                                                                         \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)

int manifest_selftest(void) {
    /* Runs from core_services_init, before any process exists (the
     * cap_selftest precedent: scratch pids 1 and 2, cleaned up after). */
    manifest_t m;
    man_memset(&m, 0, sizeof(m));
    m.bound = 1;
    m.entry_count = 1;
    m.entries[0].resource_type = CAP_RESOURCE_MEMORY;
    m.entries[0].resource_id = 42;
    m.entries[0].permissions = CAP_READ;
    m.spawn_max = 1;
    manifest_bind(1, &m);

    /* Covered row allows; uncovered resource must MDENY through the REAL
     * check helper (this records the boot AUDIT_MDENY the CI gate parses —
     * on the shipped system no citizen ever trips this path, so this is
     * what keeps a constant-true manifest_check from shipping green). */
    MST_ASSERT(manifest_check(1, CAP_RESOURCE_MEMORY, 42, CAP_READ) == 1, "covered row denied");
    MST_ASSERT(manifest_check(1, CAP_RESOURCE_DEVICE, 7, CAP_WRITE) == 0, "uncovered row allowed");

    /* Unbound = allow. */
    MST_ASSERT(manifest_check(2, CAP_RESOURCE_DEVICE, 7, CAP_WRITE) == 1, "unbound pid denied");

    /* Spawn quota: one success fits, the second refusal records AUDIT_QUOTA
     * through the same precheck the spawn syscall uses. */
    MST_ASSERT(manifest_spawn_precheck(1) == 1, "quota refused under limit");
    manifest_spawn_charge(1);
    MST_ASSERT(manifest_spawn_precheck(1) == 0, "quota allowed over limit");

    /* Rebind overwrites (last-write-wins — the start_slot-over-finalize
     * ordering depends on it), clear restores unbound-allow. */
    m.spawn_max = 0;
    manifest_bind(1, &m);
    MST_ASSERT(manifest_spawn_precheck(1) == 1, "rebind did not overwrite");
    manifest_clear(1);
    MST_ASSERT(manifest_check(1, CAP_RESOURCE_DEVICE, 7, CAP_WRITE) == 1, "clear did not unbind");

    boot_log("manifest self-test: PASS (MDENY recorded)");
    return 0;
}
