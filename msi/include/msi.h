#ifndef MSI_H
#define MSI_H

#include <stdint.h>
#include <stddef.h>

// MSI Version
#define MSI_VERSION_MAJOR 1
#define MSI_VERSION_MINOR 0
#define MSI_VERSION_PATCH 0

// MSI Result Codes
typedef enum {
    MSI_SUCCESS = 0,
    MSI_ERROR_INVALID_ARG = -1,
    MSI_ERROR_NO_MEMORY = -2,
    MSI_ERROR_PERMISSION_DENIED = -3,
    MSI_ERROR_TIMEOUT = -4,
    MSI_ERROR_NOT_FOUND = -5,
    MSI_ERROR_QUANTUM_DECOHERED = -6,
    MSI_ERROR_ASSOC_COLLISION = -7
} msi_result_t;

// Forward declarations
typedef struct msi_domain msi_domain_t;
typedef struct msi_lane msi_lane_t;
typedef struct msi_event msi_event_t;
typedef uint32_t msi_topic_t;

// System information structure
typedef struct {
    uint32_t version;
    uint32_t capabilities;
    char vendor[32];
} msi_system_info_t;

// Associative memory entry
typedef struct {
    uint8_t *vector;
    size_t dimensions;
    void *payload;
    size_t payload_size;
} msi_assoc_entry_t;

// State mapping flags
#define MSI_STATE_READ    0x01
#define MSI_STATE_WRITE   0x02
#define MSI_STATE_EXECUTE 0x04
#define MSI_STATE_SHARED  0x08

// === Discovery Operations ===
msi_result_t msi_version(msi_system_info_t *info);
msi_result_t msi_capabilities(uint32_t *caps);
msi_result_t msi_attest(const uint8_t *challenge, uint8_t *response);

// === Domain Operations ===
msi_result_t msi_domain_create(msi_domain_t **domain);
msi_result_t msi_domain_grant(msi_domain_t *domain, uint32_t capability);
msi_result_t msi_domain_seal(msi_domain_t *domain);
msi_result_t msi_domain_destroy(msi_domain_t *domain);

// === Lane Operations ===
msi_result_t msi_lane_spawn(msi_domain_t *domain, msi_lane_t **lane);
msi_result_t msi_lane_yield(msi_lane_t *lane);
msi_result_t msi_lane_sleep(msi_lane_t *lane, uint64_t nanos);
msi_result_t msi_lane_kill(msi_lane_t *lane);

// === Event Operations ===
msi_result_t msi_event_publish(msi_topic_t topic, const void *data, size_t len);
msi_result_t msi_event_subscribe(msi_topic_t topic, msi_lane_t *lane);
msi_result_t msi_event_wait(msi_lane_t *lane, msi_event_t **event, uint64_t timeout_ns);

// === State Operations (Addressable Memory) ===
msi_result_t msi_state_map(void **addr, size_t size, uint32_t flags);
msi_result_t msi_state_read(const void *addr, void *buf, size_t len);
msi_result_t msi_state_write(void *addr, const void *data, size_t len);
msi_result_t msi_state_commit(void *addr, size_t size);

// === Associative Memory Operations ===
msi_result_t msi_assoc_put(const msi_assoc_entry_t *entry);
msi_result_t msi_assoc_get(const uint8_t *vector, msi_assoc_entry_t *result);
msi_result_t msi_assoc_query(const uint8_t *vector, msi_assoc_entry_t *results, size_t max_results);
msi_result_t msi_assoc_forget(const uint8_t *vector);

#endif // MSI_H
