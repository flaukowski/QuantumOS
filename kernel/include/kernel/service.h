/**
 * QuantumOS User-Space Services Framework (#6)
 *
 * Service lifecycle management following the microkernel philosophy:
 * services are isolated processes managed by a central service
 * manager with registration, dependency-ordered startup, health
 * monitoring, and automatic restart.
 *
 * Until user mode lands, services run as PROCESS_TYPE_SERVICE kernel
 * threads — the manager, registry, and monitoring semantics are the
 * same; only the isolation boundary strengthens later.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SERVICE_H
#define SERVICE_H

#include <kernel/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* 24, not 16: the boot roster reached 15/16 with quota-test (epic #135), and
 * service_register exhaustion is only a boot_log warning — the next citizen
 * would fail with no obvious cause. Headroom is cheap (one slot ~= 700 B). */
#define MAX_SERVICES 24
#define SERVICE_NAME_MAX 64
#define SERVICE_MAX_DEPS 8
#define SERVICE_DEFAULT_MAX_RESTARTS 3

/* Heartbeat freshness threshold (timer ticks; 10 ms each) */
#define SERVICE_HEARTBEAT_TIMEOUT 200 /* 2 s */
/* How often the health monitor scans (timer ticks) */
#define SERVICE_MONITOR_INTERVAL 100 /* 1 s */

/* Service states (issue #6 spec) */
typedef enum {
    SERVICE_STATE_STOPPED = 0,
    SERVICE_STATE_STARTING,
    SERVICE_STATE_RUNNING,
    SERVICE_STATE_STOPPING,
    SERVICE_STATE_CRASHED
} service_state_t;

/* Result codes */
typedef enum {
    SVC_SUCCESS = 0,
    SVC_ERROR_NOT_FOUND = -1,
    SVC_ERROR_ALREADY_RUNNING = -2,
    SVC_ERROR_NO_SPACE = -3,
    SVC_ERROR_DEPENDENCY = -4,
    SVC_ERROR_DEPENDENCY_CYCLE = -5,
    SVC_ERROR_SPAWN_FAILED = -6,
    SVC_ERROR_INVALID_ARG = -7,
    SVC_ERROR_RESTART_LIMIT = -8
} svc_result_t;

/* ============================================================================
 * Structures
 * ============================================================================ */

/* Runtime service record (issue #6 spec) */
typedef struct {
    uint32_t service_id;
    char name[SERVICE_NAME_MAX];
    uint32_t pid;
    uint32_t pid_generation; /* process_get_generation(pid) at spawn; guards
                              * against acting on a recycled pid after the
                              * idle reaper freed our PCB (phase 3 hardening) */
    uint32_t state;          /* service_state_t */
    uint32_t capabilities;   /* CAP_RESOURCE_SERVICE cap_id */
    uint64_t start_time;     /* timer ticks */
    uint32_t restart_count;
    uint32_t max_restarts;
    uint32_t cpu_limit; /* quota fields; enforcement TODO */
    size_t memory_limit;
    uint32_t quantum_limit;
    char dependencies[SERVICE_MAX_DEPS][SERVICE_NAME_MAX];
} service_info_t;

/* Static registration record.
 *
 * A service is either a kernel thread (set `entry`) or an isolated
 * ring-3 user process loaded from an embedded ELF image (set
 * `user_elf_start`/`user_elf_end`, leave `entry` NULL). */
typedef struct {
    const char *name;
    void (*entry)(void);        /* kernel-thread main loop; must not return */
    const void *user_elf_start; /* user-process service: embedded ELF image */
    const void *user_elf_end;
    const char *dependencies[SERVICE_MAX_DEPS]; /* NULL-terminated list */
    uint32_t max_restarts;
    uint32_t cpu_limit;
    size_t memory_limit;
    uint32_t quantum_limit;
    /* Resource caps to (re-)mint on every start, so a watchdog restart
     * restores the same authority the service had at first spawn instead of
     * silently degrading. Currently: a CAP_RESOURCE_QUANTUM read cap on the
     * shared pool (e.g. ghostd's perturbation-noise source). */
    uint8_t grant_quantum_pool;
    /* Grant a CAP_RESOURCE_DEVICE read/write cap over the COM2 swarm-bridge
     * UART (DEVICE_ID_COM2), re-minted on every start like grant_quantum_pool.
     * Held only by swarm_svc, so it is the sole ring-3 process that can drive
     * the serial bridge (ghostd phase 4). */
    uint8_t grant_com2;
    /* Grant a CAP_RESOURCE_DEVICE read/write cap over the interactive
     * console (DEVICE_ID_CONSOLE), re-minted on every start. Held only by
     * qsh, so the shell is the sole ring-3 process that can read keystrokes
     * or write raw console bytes (epic #62 phase 1). */
    uint8_t grant_console;
    /* Grant a CAP_RESOURCE_PROCESS execute cap over SPAWN_RESOURCE_ID,
     * re-minted on every start. The spawn right: SYS_SPAWN starts initrd
     * programs only for holders. Held only by qsh (epic #62 phase 3). */
    uint8_t grant_spawn;
    /* Grant a CAP_RESOURCE_DEVICE write cap over DEVICE_ID_DISK, re-minted
     * on every start. The filesystem-write right: create/append/unlink on
     * the RAM overlay and SYS_SYNC to the persistence disk. Held only by
     * qsh (epic #71). */
    uint8_t grant_fswrite;
    /* Grant a CAP_RESOURCE_DEVICE read cap over DEVICE_ID_NET, re-minted on
     * every start. The network-access right: SYS_RESOLVE (hostname lookup)
     * from ring 3. Held only by qsh (epic #73 follow-up). */
    uint8_t grant_net;
    /* Grant a CAP_RESOURCE_FIELD read+write cap over field_region,
     * re-minted on every start. The holographic-memory right: SYS_IMPRINT
     * and SYS_RECALL against exactly that region (epic #95). The region is
     * SCRUBBED at every (re)mint — a reborn or successor service must
     * never inherit its predecessor's memories. */
    uint8_t grant_field;
    uint8_t field_region; /* which region the field cap names (< FIELD_REGION_COUNT) */
    /* Opt in to inheriting DISK-restored region content at the FIRST
     * grant of a boot (epic #96). Without it every grant scrubs (the
     * epic #95 rule). Inheritance is consumed once per boot per region;
     * watchdog rebirths and successors always scrub. qsh only. */
    uint8_t field_inherit;
    /* Grant CAP_GRANT alongside grant_field (epic #137): the field cap over
     * field_region is minted CAP_READ|CAP_WRITE|CAP_GRANT so this citizen —
     * and ONLY this citizen — may cap_derive a NARROWED slice of that region
     * to a sub-agent via SYS_CAP_DERIVE. Keeps the CAP_GRANT blast radius to a
     * single auditable delegator; qsh deliberately does NOT set it. */
    uint8_t grant_field_delegable;
    /* Manifest spawn quota (epic #135): successful SYS_SPAWNs allowed per
     * incarnation (a watchdog rebirth re-binds the manifest, resetting the
     * counter — a soft, per-incarnation bound). 0 = bound but unlimited.
     * Only meaningful alongside grant_spawn. quota-test proves enforcement
     * with spawn_max=1; qsh stays 0 — a finite shell quota would brick
     * long-lived agent sessions no CI boot can ever exercise. */
    uint32_t spawn_max;
    /* Grant a CAP_RESOURCE_DEVICE WRITE cap over DEVICE_ID_QPU, re-minted on
     * every start (epic #148): the QPU SUBMIT right — queue an opaque circuit
     * for execution and POLL the result. Held by submitters (qpu_test now,
     * qsh later). WRITE and EXECUTE must NEVER co-occur on a QPU cap. */
    uint8_t grant_qpu_submit;
    /* Grant a CAP_RESOURCE_DEVICE READ|EXECUTE cap over DEVICE_ID_QPU, the
     * QPU EXECUTOR right (FETCH pending jobs + COMPLETE them). Held ONLY by
     * qpud, the single executor. Never mints CAP_WRITE. */
    uint8_t grant_qpu_execute;
    /* Manifest QPU-submission quota (epic #148): accepted SYS_QPU submissions
     * allowed per incarnation (rebirth resets, mirroring spawn_max). 0 =
     * unlimited. Only meaningful alongside grant_qpu_submit. qpu_test proves
     * enforcement with qsub_max=2. */
    uint32_t qsub_max;
} service_definition_t;

/* ============================================================================
 * Service Manager API (issue #6 spec)
 * ============================================================================ */

svc_result_t service_manager_init(void);

/* Register a service definition (does not start it) */
svc_result_t service_register(const service_definition_t *def, uint32_t *service_id_out);

/* Start a service by name; dependencies are started first,
 * dependency cycles are refused */
svc_result_t service_start(const char *name, const char *args);

svc_result_t service_stop(uint32_t service_id);
svc_result_t service_restart(uint32_t service_id);
svc_result_t service_status(uint32_t service_id, service_info_t *info);
svc_result_t service_list(service_info_t *services, size_t max_count, uint32_t *count);

/* Enable/disable health monitoring (heartbeat watchdog + auto-restart) */
svc_result_t service_monitor(uint32_t service_id, bool enable);

/* Called by a service from its own thread to report liveness */
void service_heartbeat(void);

/* Restart count of the currently running service (looked up by the
 * current pid), or -1 if the caller is not a running service. Lets a
 * service detect that the watchdog restarted it (rebirth). */
int32_t service_current_restart_count(void);

/* Look up a service id by name */
svc_result_t service_find(const char *name, uint32_t *service_id_out);

/* True if `pid` is a currently-RUNNING, health-MONITORED service. Used by
 * SYS_CAP_DERIVE (epic #137) to refuse delegating to a monitored service: its
 * watchdog restart rebinds its manifest from grant flags and cascade-revokes
 * the derived cap, so a delegated row would silently evaporate. */
bool service_pid_is_monitored(uint32_t pid);

/* Boot self-test: registers demo services, verifies dependency-ordered
 * startup and the status/list API */
svc_result_t service_selftest(void);

#endif /* SERVICE_H */
