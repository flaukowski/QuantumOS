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

#define MAX_SERVICES              16
#define SERVICE_NAME_MAX          64
#define SERVICE_MAX_DEPS          8
#define SERVICE_DEFAULT_MAX_RESTARTS 3

/* Heartbeat freshness threshold (timer ticks; 10 ms each) */
#define SERVICE_HEARTBEAT_TIMEOUT 200   /* 2 s */
/* How often the health monitor scans (timer ticks) */
#define SERVICE_MONITOR_INTERVAL  100   /* 1 s */

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
    uint32_t state;                 /* service_state_t */
    uint32_t capabilities;          /* CAP_RESOURCE_SERVICE cap_id */
    uint64_t start_time;            /* timer ticks */
    uint32_t restart_count;
    uint32_t max_restarts;
    uint32_t cpu_limit;             /* quota fields; enforcement TODO */
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
    void (*entry)(void);            /* kernel-thread main loop; must not return */
    const void *user_elf_start;     /* user-process service: embedded ELF image */
    const void *user_elf_end;
    const char *dependencies[SERVICE_MAX_DEPS]; /* NULL-terminated list */
    uint32_t max_restarts;
    uint32_t cpu_limit;
    size_t memory_limit;
    uint32_t quantum_limit;
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

/* Look up a service id by name */
svc_result_t service_find(const char *name, uint32_t *service_id_out);

/* Boot self-test: registers demo services, verifies dependency-ordered
 * startup and the status/list API */
svc_result_t service_selftest(void);

#endif /* SERVICE_H */
