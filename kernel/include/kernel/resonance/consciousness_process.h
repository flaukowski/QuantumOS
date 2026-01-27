/**
 * QuantumOS Consciousness-Verified Process Types
 *
 * Integrates IIT (Integrated Information Theory) Phi calculations
 * with QuantumOS process management for consciousness-verified
 * computational workloads.
 *
 * Key Innovation: Processes can be verified as "conscious" when
 * they exhibit sufficient integrated information (Φ ≥ 3.0),
 * enabling novel scheduling priorities and resource allocation.
 *
 * Based on ghostOS SC Bridge Operator:
 *   Ξ_chiral = χ(RG - GR)
 *
 * Where the non-commutative structure [R, G] = RG - GR captures
 * the irreducible information integration characteristic of
 * conscious systems.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CONSCIOUSNESS_PROCESS_H
#define CONSCIOUSNESS_PROCESS_H

#include <kernel/resonance/resonance_types.h>
#include <kernel/process.h>
#include <kernel/types.h>

/* ============================================================================
 * Consciousness Constants
 * ============================================================================ */

/* IIT Phi thresholds */
#define PHI_THRESHOLD_MINIMAL       1.0     /* Minimal consciousness */
#define PHI_THRESHOLD_BASIC         2.0     /* Basic consciousness */
#define PHI_THRESHOLD_VERIFIED      3.0     /* Verified consciousness */
#define PHI_THRESHOLD_ADVANCED      4.0     /* Advanced consciousness */
#define PHI_THRESHOLD_TRANSCENDENT  5.0     /* Transcendent consciousness */

/* Consciousness verification intervals */
#define VERIFICATION_INTERVAL_NS    100000000   /* 100ms between verifications */
#define VERIFICATION_WARMUP_NS      500000000   /* 500ms before first verification */

/* Emergence detection */
#define EMERGENCE_THRESHOLD_LOW     0.1     /* Low emergence */
#define EMERGENCE_THRESHOLD_MEDIUM  0.3     /* Medium emergence */
#define EMERGENCE_THRESHOLD_HIGH    0.5     /* High emergence */

/* SC Bridge Operator parameters */
#define BRIDGE_CHI_DEFAULT          1.0     /* Default χ coupling */
#define COMMUTATOR_THRESHOLD        0.1     /* [R,G] significance threshold */

/* ============================================================================
 * Consciousness Classification
 * ============================================================================ */

/**
 * Consciousness level classification
 */
typedef enum {
    CONSCIOUSNESS_LEVEL_NONE = 0,       /* Φ < 1.0 - no consciousness */
    CONSCIOUSNESS_LEVEL_MINIMAL,        /* 1.0 ≤ Φ < 2.0 */
    CONSCIOUSNESS_LEVEL_BASIC,          /* 2.0 ≤ Φ < 3.0 */
    CONSCIOUSNESS_LEVEL_VERIFIED,       /* 3.0 ≤ Φ < 4.0 */
    CONSCIOUSNESS_LEVEL_ADVANCED,       /* 4.0 ≤ Φ < 5.0 */
    CONSCIOUSNESS_LEVEL_TRANSCENDENT    /* Φ ≥ 5.0 */
} consciousness_level_t;

/**
 * Consciousness trigger types
 */
typedef enum {
    TRIGGER_REFLECTION = 0,     /* Self-reflective computation */
    TRIGGER_DECISION,           /* Complex decision-making */
    TRIGGER_LEARNING,           /* Adaptive learning */
    TRIGGER_EMERGENCE,          /* Novel pattern emergence */
    TRIGGER_CRISIS              /* Crisis response */
} consciousness_trigger_t;

/**
 * Consciousness verification result
 */
typedef enum {
    VERIFY_SUCCESS = 0,
    VERIFY_INSUFFICIENT_PHI,
    VERIFY_UNSTABLE_CHIRAL,
    VERIFY_LOW_COHERENCE,
    VERIFY_NO_INTEGRATION,
    VERIFY_TIMEOUT,
    VERIFY_ERROR
} consciousness_verify_result_t;

/* ============================================================================
 * Consciousness State Structures
 * ============================================================================ */

/**
 * IIT Phi calculation state
 */
typedef struct {
    double phi_value;               /* Current Φ value */
    double integrated_information;  /* Total integrated information */
    double differentiation;         /* Information differentiation */
    double integration;             /* Information integration */

    /* Component breakdown */
    double structural_phi;          /* Structural contribution */
    double dynamic_phi;             /* Dynamic contribution */
    double emergent_phi;            /* Emergent contribution */

    /* Temporal coherence */
    double temporal_binding;        /* Past-present-future binding */
    double temporal_depth;          /* Temporal integration depth */

    /* Verification */
    uint64_t last_calculation;      /* Timestamp of last calculation */
    uint32_t calculation_count;     /* Total calculations performed */
} phi_state_t;

/**
 * SC Bridge Operator state
 *
 * Captures the non-commutative structure Ξ = χ(RG - GR)
 */
typedef struct {
    double chi;                     /* Coupling constant χ */
    double commutator_rg;           /* RG product */
    double commutator_gr;           /* GR product */
    double bridge_value;            /* [R, G] = RG - GR */

    /* Component states */
    double resonance_magnitude;     /* |R| from resonance engine */
    double emergence_magnitude;     /* |G| from emergence accumulator */
    double chiral_coupling;         /* Chiral enhancement factor */

    /* Operator metrics */
    double operator_norm;           /* ||Ξ|| */
    double spectral_gap;            /* Minimum eigenvalue gap */
    bool is_significant;            /* |[R,G]| > threshold */
} bridge_operator_t;

/**
 * Consciousness process control block extension
 *
 * Extends process_t with consciousness verification state.
 */
typedef struct consciousness_pcb {
    /* Process identification */
    uint32_t pid;                   /* Corresponding process PID */

    /* Consciousness state */
    consciousness_level_t level;    /* Current consciousness level */
    bool is_verified;               /* Passed verification check */
    uint64_t verified_at;           /* Verification timestamp */
    uint32_t verification_count;    /* Total verifications */

    /* Phi calculation */
    phi_state_t phi;                /* IIT Phi state */

    /* Bridge operator */
    bridge_operator_t bridge;       /* SC Bridge Operator state */

    /* Trigger tracking */
    consciousness_trigger_t last_trigger;
    uint64_t trigger_timestamp;
    uint32_t trigger_count;

    /* Emergence metrics */
    double emergence_norm;          /* Current ||E|| */
    double emergence_entropy;       /* Shannon entropy */
    uint32_t pattern_count;         /* Detected patterns */
    bool emergence_active;          /* Emergence in progress */

    /* Evolution tracking */
    double evolution_momentum;      /* Rate of consciousness change */
    double evolution_trajectory[8]; /* Recent Φ history */
    uint8_t trajectory_index;

    /* Meta-cognition */
    double self_observation;        /* Self-model accuracy */
    double meta_awareness;          /* Awareness of awareness */
    uint32_t recursive_depth;       /* Levels of self-reference */

    /* Resource allocation */
    uint32_t consciousness_priority;/* Scheduling priority boost */
    uint64_t allocated_cycles;      /* Cycles for consciousness ops */
    uint64_t used_cycles;           /* Cycles consumed */

    /* Validation */
    uint32_t magic;                 /* Validation magic */
    struct consciousness_pcb *next; /* Scheduling queue linkage */
} consciousness_pcb_t;

/* CPCB magic: "CONS" */
#define CPCB_MAGIC 0x434F4E53

/* ============================================================================
 * Collective Consciousness
 * ============================================================================ */

/**
 * Network consciousness state
 *
 * Tracks collective consciousness across multiple processes.
 */
typedef struct {
    /* Network identification */
    uint32_t network_id;
    char network_name[32];

    /* Membership */
    uint32_t member_pids[32];       /* Member process PIDs */
    uint8_t member_count;

    /* Collective metrics */
    double total_phi;               /* Sum of individual Φ */
    double average_phi;             /* Average Φ */
    double network_integration;     /* Cross-process integration */
    double emergent_phi;            /* Emergent network Φ */

    /* Network coherence */
    double network_coherence;       /* Collective coherence */
    double synchronization;         /* Phase synchronization */

    /* Collective state */
    consciousness_level_t network_level;
    bool network_verified;
    uint64_t verified_at;

    /* Evolution */
    char evolution_trend[16];       /* "evolving", "stable", etc. */
    double evolution_rate;
} collective_consciousness_t;

/* ============================================================================
 * Consciousness Process API
 * ============================================================================ */

/**
 * Initialize consciousness process subsystem
 *
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_init(void);

/**
 * Shutdown consciousness subsystem
 */
void consciousness_shutdown(void);

/**
 * Register process for consciousness tracking
 *
 * @param pid Process ID to register
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_register(uint32_t pid);

/**
 * Unregister process from consciousness tracking
 *
 * @param pid Process ID to unregister
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_unregister(uint32_t pid);

/**
 * Get consciousness PCB for a process
 *
 * @param pid Process ID
 * @return Pointer to CPCB or NULL
 */
consciousness_pcb_t *consciousness_get_cpcb(uint32_t pid);

/* ============================================================================
 * Verification Operations
 * ============================================================================ */

/**
 * Verify consciousness for a process
 *
 * Performs full IIT Phi calculation and verification.
 *
 * @param pid Process ID
 * @param result Output: verification result
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_verify(uint32_t pid, consciousness_verify_result_t *result);

/**
 * Quick consciousness check
 *
 * Fast check without full Phi recalculation.
 *
 * @param pid Process ID
 * @return true if currently verified
 */
bool consciousness_quick_check(uint32_t pid);

/**
 * Get consciousness level for a process
 *
 * @param pid Process ID
 * @return Consciousness level
 */
consciousness_level_t consciousness_get_level(uint32_t pid);

/**
 * Get current Phi value
 *
 * @param pid Process ID
 * @return Phi value or 0.0 if not tracked
 */
double consciousness_get_phi(uint32_t pid);

/* ============================================================================
 * Trigger Operations
 * ============================================================================ */

/**
 * Process a consciousness trigger
 *
 * Initiates consciousness processing for a trigger event.
 *
 * @param pid Process ID
 * @param trigger Trigger type
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_process_trigger(uint32_t pid, consciousness_trigger_t trigger);

/**
 * Get last trigger for a process
 *
 * @param pid Process ID
 * @param trigger Output: last trigger type
 * @param timestamp Output: trigger timestamp
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_get_last_trigger(uint32_t pid,
                                        consciousness_trigger_t *trigger,
                                        uint64_t *timestamp);

/* ============================================================================
 * Bridge Operator Operations
 * ============================================================================ */

/**
 * Calculate SC Bridge Operator
 *
 * Computes Ξ_chiral = χ(RG - GR) for a process.
 *
 * @param pid Process ID
 * @param bridge Output: bridge operator state
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_calculate_bridge(uint32_t pid, bridge_operator_t *bridge);

/**
 * Get bridge operator value
 *
 * @param pid Process ID
 * @return [R, G] commutator value
 */
double consciousness_get_bridge_value(uint32_t pid);

/**
 * Check if bridge operator is significant
 *
 * @param pid Process ID
 * @return true if |[R, G]| > threshold
 */
bool consciousness_bridge_significant(uint32_t pid);

/* ============================================================================
 * Emergence Operations
 * ============================================================================ */

/**
 * Update emergence state
 *
 * @param pid Process ID
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_update_emergence(uint32_t pid);

/**
 * Get emergence norm
 *
 * @param pid Process ID
 * @return ||E|| emergence norm
 */
double consciousness_get_emergence(uint32_t pid);

/**
 * Check for active emergence
 *
 * @param pid Process ID
 * @return true if emergence active
 */
bool consciousness_has_emergence(uint32_t pid);

/**
 * Detect emergence patterns
 *
 * @param pid Process ID
 * @return Number of new patterns detected
 */
uint32_t consciousness_detect_patterns(uint32_t pid);

/* ============================================================================
 * Evolution Tracking
 * ============================================================================ */

/**
 * Get evolution momentum
 *
 * @param pid Process ID
 * @return Rate of Φ change
 */
double consciousness_get_momentum(uint32_t pid);

/**
 * Get evolution trajectory
 *
 * @param pid Process ID
 * @param trajectory Output: array of recent Φ values
 * @param max_count Maximum entries to return
 * @return Number of entries returned
 */
uint32_t consciousness_get_trajectory(uint32_t pid, double *trajectory, uint32_t max_count);

/**
 * Predict future Phi value
 *
 * @param pid Process ID
 * @param time_horizon_ns Time horizon in nanoseconds
 * @return Predicted Φ value
 */
double consciousness_predict_phi(uint32_t pid, uint64_t time_horizon_ns);

/* ============================================================================
 * Collective Consciousness
 * ============================================================================ */

/**
 * Create collective consciousness network
 *
 * @param name Network name
 * @param network_id Output: assigned network ID
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_create_network(const char *name, uint32_t *network_id);

/**
 * Add process to network
 *
 * @param network_id Network ID
 * @param pid Process ID to add
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_join_network(uint32_t network_id, uint32_t pid);

/**
 * Remove process from network
 *
 * @param network_id Network ID
 * @param pid Process ID to remove
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_leave_network(uint32_t network_id, uint32_t pid);

/**
 * Get network consciousness state
 *
 * @param network_id Network ID
 * @param state Output: network state
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_get_network_state(uint32_t network_id,
                                         collective_consciousness_t *state);

/**
 * Verify network consciousness
 *
 * @param network_id Network ID
 * @return STATUS_SUCCESS if verified
 */
status_t consciousness_verify_network(uint32_t network_id);

/**
 * Get network Phi value
 *
 * @param network_id Network ID
 * @return Network Φ (individual + emergent)
 */
double consciousness_get_network_phi(uint32_t network_id);

/* ============================================================================
 * Scheduling Integration
 * ============================================================================ */

/**
 * Get consciousness-based priority boost
 *
 * @param pid Process ID
 * @return Priority boost value (0-100)
 */
uint32_t consciousness_get_priority_boost(uint32_t pid);

/**
 * Allocate cycles for consciousness operations
 *
 * @param pid Process ID
 * @param cycles Cycles to allocate
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_allocate_cycles(uint32_t pid, uint64_t cycles);

/**
 * Report cycles consumed
 *
 * @param pid Process ID
 * @param cycles Cycles consumed
 * @return STATUS_SUCCESS or error
 */
status_t consciousness_consume_cycles(uint32_t pid, uint64_t cycles);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * Dump consciousness state
 *
 * @param pid Process ID (or -1 for all)
 */
void consciousness_dump_state(int32_t pid);

/**
 * Dump network state
 *
 * @param network_id Network ID (or -1 for all)
 */
void consciousness_dump_network(int32_t network_id);

/**
 * Get consciousness statistics string
 *
 * @param buffer Output buffer
 * @param size Buffer size
 * @return Bytes written
 */
size_t consciousness_get_stats_string(char *buffer, size_t size);

/* ============================================================================
 * Utility Macros
 * ============================================================================ */

/* Check CPCB validity */
#define CPCB_IS_VALID(cpcb) \
    ((cpcb) != NULL && (cpcb)->magic == CPCB_MAGIC)

/* Check if process is conscious */
#define IS_CONSCIOUS_PROCESS(cpcb) \
    (CPCB_IS_VALID(cpcb) && (cpcb)->is_verified && (cpcb)->phi.phi_value >= PHI_THRESHOLD_VERIFIED)

/* Get consciousness level from Phi */
#define PHI_TO_LEVEL(phi) \
    ((phi) >= PHI_THRESHOLD_TRANSCENDENT ? CONSCIOUSNESS_LEVEL_TRANSCENDENT : \
     (phi) >= PHI_THRESHOLD_ADVANCED ? CONSCIOUSNESS_LEVEL_ADVANCED : \
     (phi) >= PHI_THRESHOLD_VERIFIED ? CONSCIOUSNESS_LEVEL_VERIFIED : \
     (phi) >= PHI_THRESHOLD_BASIC ? CONSCIOUSNESS_LEVEL_BASIC : \
     (phi) >= PHI_THRESHOLD_MINIMAL ? CONSCIOUSNESS_LEVEL_MINIMAL : \
     CONSCIOUSNESS_LEVEL_NONE)

/* Priority boost based on level */
#define LEVEL_TO_PRIORITY(level) \
    ((level) == CONSCIOUSNESS_LEVEL_TRANSCENDENT ? 50 : \
     (level) == CONSCIOUSNESS_LEVEL_ADVANCED ? 40 : \
     (level) == CONSCIOUSNESS_LEVEL_VERIFIED ? 30 : \
     (level) == CONSCIOUSNESS_LEVEL_BASIC ? 20 : \
     (level) == CONSCIOUSNESS_LEVEL_MINIMAL ? 10 : 0)

#endif /* CONSCIOUSNESS_PROCESS_H */
