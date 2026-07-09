/**
 * QuantumOS Capability Authority Ledger implementation (epic #133 — Phase D).
 * See kernel/include/kernel/audit.h for the model, scope, and concurrency rule.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/interrupts.h> /* timer_get_ticks */

_Static_assert(sizeof(audit_entry_t) == 40, "audit entry ABI drift");
_Static_assert(AUDIT_LOG_ENTRIES *AUDIT_LINE_MAX == AUDIT_MAX_BYTES, "audit buffer sizing");

/* The ring lives in .bss (zeroed → every slot is kind AUDIT_KIND_EMPTY until
 * written). `total` is the monotonic count of records ever ACCEPTED as new
 * (coalesced repeats do NOT advance it — they bump the last entry's count), so
 * seq == the entry's total-at-write and strictly increases down the log. */
static audit_entry_t ring[AUDIT_LOG_ENTRIES];
static uint64_t total; /* new records ever accepted; also the next seq */

/* Self-contained IRQ save/restore: audit_record is called from syscall context
 * AND the IF=1 service health monitor (service.c), so it must not assume a cli'd
 * caller. Single CPU, so disabling interrupts around the ~6-field append is a
 * sufficient mutual-exclusion (mirrors service.c's svc_irq_save). */
static inline uint64_t audit_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void audit_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

void audit_record(uint16_t kind, uint32_t pid, uint16_t verdict, uint32_t resource_type,
                  uint32_t resource_id, uint32_t permissions) {
    uint64_t flags = audit_irq_save();

    /* Coalesce: an identical record repeated back-to-back (a denial flood) bumps
     * the most-recent entry's count instead of appending, so it cannot evict
     * older GRANT/DENY evidence. `total`/seq do not advance on a coalesce. */
    if (total > 0) {
        audit_entry_t *last = &ring[(uint32_t)((total - 1) % AUDIT_LOG_ENTRIES)];
        if (last->kind == kind && last->pid == pid && last->verdict == verdict &&
            last->resource_type == resource_type && last->resource_id == resource_id &&
            last->permissions == permissions) {
            last->count++;
            last->tick = timer_get_ticks(); /* refresh to the latest occurrence */
            audit_irq_restore(flags);
            return;
        }
    }

    audit_entry_t *e = &ring[(uint32_t)(total % AUDIT_LOG_ENTRIES)];
    e->seq = total;
    e->tick = timer_get_ticks();
    e->pid = pid;
    e->kind = kind;
    e->verdict = verdict;
    e->resource_type = resource_type;
    e->resource_id = resource_id;
    e->permissions = permissions;
    e->count = 1;
    total++;

    audit_irq_restore(flags);
}

void audit_grant(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions) {
    audit_record(AUDIT_GRANT, pid, AUDIT_V_OK, resource_type, resource_id, permissions);
}

void audit_deny(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions) {
    audit_record(AUDIT_DENY, pid, AUDIT_V_EPERM, resource_type, resource_id, permissions);
}

void audit_spawn(uint32_t parent_pid, uint32_t child_pid) {
    /* resource_id carries the child pid; PROCESS/EXECUTE mirrors the spawn cap. */
    audit_record(AUDIT_SPAWN, parent_pid, AUDIT_V_OK, CAP_RESOURCE_PROCESS, child_pid, CAP_EXECUTE);
}

/* ---- freestanding formatting (no libc) ---- */
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

static const char *kind_name(uint16_t k) {
    switch (k) {
    case AUDIT_GRANT:
        return "GRANT";
    case AUDIT_DENY:
        return "DENY";
    case AUDIT_SPAWN:
        return "SPAWN";
    case AUDIT_MDENY:
        return "MDENY";
    case AUDIT_QUOTA:
        return "QUOTA";
    case AUDIT_CPUKILL:
        return "CPUKILL";
    default:
        return "EMPTY";
    }
}

static const char *type_name(uint32_t t) {
    /* Mirrors cap_resource_type_t (capability.h). */
    static const char *const names[] = {"MEM", "IPC", "DEV", "QRNG", "PROC", "SVC", "FIELD"};
    return t < (sizeof(names) / sizeof(names[0])) ? names[t] : "?";
}

static size_t format_entry(char *b, size_t o, size_t max, const audit_entry_t *e) {
    o = put_str(b, o, max, "AUDIT: seq=");
    o = put_u64(b, o, max, e->seq);
    o = put_str(b, o, max, " tick=");
    o = put_u64(b, o, max, e->tick);
    o = put_str(b, o, max, " pid=");
    o = put_u64(b, o, max, e->pid);
    o = put_str(b, o, max, " kind=");
    o = put_str(b, o, max, kind_name(e->kind));
    o = put_str(b, o, max, " res=");
    o = put_str(b, o, max, type_name(e->resource_type));
    o = put_str(b, o, max, ":");
    if (e->resource_id == AUDIT_RESOURCE_ANY) {
        o = put_str(b, o, max, "*");
    } else {
        o = put_u64(b, o, max, e->resource_id);
    }
    o = put_str(b, o, max, " perms=");
    o = put_hex8(b, o, max, e->permissions);
    o = put_str(b, o, max, " verdict=");
    o = put_str(b, o, max, e->verdict == AUDIT_V_EPERM ? "EPERM" : "OK");
    o = put_str(b, o, max, " count=");
    o = put_u64(b, o, max, e->count);
    o = put_str(b, o, max, "\r\n");
    return o;
}

size_t audit_format(char *buf, size_t max) {
    if (!buf || max == 0) {
        return 0;
    }
    uint64_t flags = audit_irq_save();
    uint64_t live = total < AUDIT_LOG_ENTRIES ? total : AUDIT_LOG_ENTRIES;
    /* Oldest first: when wrapped, the oldest lives at total % CAP. Never touch
     * an unpopulated slot (live bounds the walk). */
    uint64_t start = total <= AUDIT_LOG_ENTRIES ? 0 : (total % AUDIT_LOG_ENTRIES);
    size_t o = 0;
    for (uint64_t i = 0; i < live; i++) {
        const audit_entry_t *e = &ring[(uint32_t)((start + i) % AUDIT_LOG_ENTRIES)];
        o = format_entry(buf, o, max, e);
    }
    buf[o < max ? o : max - 1] = '\0';
    audit_irq_restore(flags);
    return o;
}

size_t audit_format_stats(char *buf, size_t max) {
    if (!buf || max == 0) {
        return 0;
    }
    uint64_t flags = audit_irq_save();
    uint64_t t = total;
    audit_irq_restore(flags);
    uint64_t dropped = t > AUDIT_LOG_ENTRIES ? t - AUDIT_LOG_ENTRIES : 0;
    size_t o = 0;
    o = put_str(buf, o, max, "AUDIT: total=");
    o = put_u64(buf, o, max, t);
    o = put_str(buf, o, max, " dropped=");
    o = put_u64(buf, o, max, dropped);
    o = put_str(buf, o, max, " capacity=");
    o = put_u64(buf, o, max, AUDIT_LOG_ENTRIES);
    o = put_str(buf, o, max, "\r\n");
    buf[o < max ? o : max - 1] = '\0';
    return o;
}
