/**
 * QuantumOS Chiral-Enhanced Quantum Resource Management
 *
 * Integrates ghostOS chiral dynamics with quantum resource allocation.
 * Provides stability guarantees through non-reciprocal coupling and
 * CISS (Chiral-Induced Spin Selectivity) coherence enhancement.
 *
 * Key Innovation: Quantum resources are allocated based on chiral
 * stability metrics, ensuring coherence preservation through
 * topological protection.
 *
 * Based on the Chiral Sine-Gordon Equation:
 *   φ_tt - φ_xx + sin(φ) = -ηφ_x - Γφ_t
 *
 * Where:
 *   η = chirality strength (optimal: φ⁻¹ ≈ 0.618)
 *   Γ = damping coefficient
 *   |η/Γ| < 1 = stable regime
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHIRAL_RESOURCES_H
#define CHIRAL_RESOURCES_H

#include <kernel/resonance/resonance_types.h>
#include <kernel/quantum_types.h>
#include <kernel/types.h>

/* ============================================================================
 * Chiral Resource Constants
 * ============================================================================ */

/* CISS enhancement factors */
#define CISS_COHERENCE_FACTOR       1.30    /* 30% coherence boost */
#define CISS_FIDELITY_FACTOR        1.15    /* 15% fidelity improvement */
#define CISS_SPIN_SELECTIVITY       0.85    /* 85% spin polarization */

/* Topological protection thresholds */
#define TOPOLOGICAL_CHARGE_MIN      0.1     /* Minimum viable charge */
#define TOPOLOGICAL_CHARGE_MAX      2.0     /* Maximum stable charge */
#define TOPOLOGICAL_ENERGY_GAP      0.05    /* Energy gap for protection */

/* Stability classification */
#define STABILITY_EXCELLENT         0.3     /* |η/Γ| < 0.3 */
#define STABILITY_GOOD              0.6     /* |η/Γ| < 0.6 */
#define STABILITY_MARGINAL          1.0     /* |η/Γ| < 1.0 */
/* |η/Γ| >= 1.0 is unstable */

/* ============================================================================
 * Chiral Resource Structures
 * ============================================================================ */

/**
 * Chiral stability classification
 */
typedef enum {
    CHIRAL_STABILITY_EXCELLENT = 0, /* |η/Γ| < 0.3 - highest protection */
    CHIRAL_STABILITY_GOOD,          /* |η/Γ| < 0.6 - good protection */
    CHIRAL_STABILITY_MARGINAL,      /* |η/Γ| < 1.0 - minimal protection */
    CHIRAL_STABILITY_UNSTABLE       /* |η/Γ| >= 1.0 - no protection */
} chiral_stability_class_t;

/**
 * Chiral-enhanced qubit state
 *
 * Extends standard qubit_handle_t with chiral protection metrics.
 */
typedef struct {
    qubit_handle_t base;            /* Standard qubit handle */

    /* Chiral enhancement */
    double chiral_eta;              /* Local chirality strength */
    double chiral_gamma;            /* Local damping coefficient */
    double asymmetry;               /* |η/Γ| ratio */
    chiral_stability_class_t stability_class;

    /* Topological protection */
    double topological_charge;      /* Conserved topological charge Q */
    double energy_gap;              /* Protection energy gap */
    bool topologically_protected;   /* Has topological protection */

    /* CISS enhancement */
    double ciss_polarization;       /* Spin polarization (0-1) */
    double ciss_coherence_boost;    /* Coherence enhancement factor */
    bool ciss_active;               /* CISS enhancement enabled */

    /* Enhanced metrics */
    uint64_t enhanced_coherence_time;/* CISS-enhanced coherence window */
    uint32_t enhanced_fidelity;     /* CISS-enhanced fidelity */

    /* Coupling state */
    handedness_t handedness;        /* Qubit coupling handedness */
    uint32_t coupled_qubits[4];     /* Chirally coupled qubit IDs */
    uint8_t coupling_count;         /* Number of couplings */
} chiral_qubit_t;

/**
 * Chiral qubit pool
 *
 * Pool of chirally-enhanced qubits with stability tracking.
 */
typedef struct {
    /* Pool inventory */
    uint32_t total_qubits;
    uint32_t available_qubits;
    uint32_t allocated_qubits;

    /* By stability class */
    uint32_t excellent_qubits;      /* Highest stability */
    uint32_t good_qubits;
    uint32_t marginal_qubits;
    uint32_t unstable_qubits;       /* Need rebalancing */

    /* By handedness */
    uint32_t left_handed_qubits;
    uint32_t right_handed_qubits;
    uint32_t neutral_qubits;

    /* CISS metrics */
    uint32_t ciss_enabled_qubits;   /* Qubits with CISS active */
    double average_ciss_boost;       /* Average coherence enhancement */

    /* Topological metrics */
    uint32_t protected_qubits;      /* Topologically protected */
    double total_topological_charge;/* Sum of charges */

    /* Global stability */
    double average_asymmetry;       /* Average |η/Γ| */
    chiral_stability_class_t pool_stability;
} chiral_pool_t;

/**
 * Chiral allocation request
 *
 * Request for chirally-enhanced qubit allocation.
 */
typedef struct {
    uint32_t pid;                   /* Requesting process */
    uint32_t qubits_requested;      /* Number of qubits needed */

    /* Stability requirements */
    chiral_stability_class_t min_stability;
    double max_asymmetry;           /* Maximum acceptable |η/Γ| */

    /* Enhancement preferences */
    bool require_ciss;              /* Require CISS enhancement */
    bool require_topological;       /* Require topological protection */
    handedness_t preferred_handedness;

    /* Coupling requirements */
    bool require_coupling;          /* Qubits must be coupleable */
    uint32_t couple_with_pid;       /* PID to couple with */

    /* Coherence requirements */
    uint64_t min_coherence_time;    /* Minimum coherence window (ns) */
    uint32_t min_fidelity;          /* Minimum fidelity (0-10000) */
} chiral_alloc_request_t;

/**
 * Chiral allocation result
 */
typedef struct {
    bool success;
    uint32_t qubits_allocated;
    uint32_t *qubit_ids;            /* Allocated qubit IDs */

    /* Achieved metrics */
    chiral_stability_class_t achieved_stability;
    double achieved_asymmetry;
    uint64_t achieved_coherence;
    uint32_t achieved_fidelity;
    double ciss_boost;

    /* Warnings */
    bool stability_compromised;     /* Couldn't meet stability requirement */
    bool coherence_reduced;         /* Coherence less than requested */
} chiral_alloc_result_t;

/* ============================================================================
 * Chiral Resource API
 * ============================================================================ */

/**
 * Initialize chiral resource management
 *
 * @return STATUS_SUCCESS or error
 */
status_t chiral_resources_init(void);

/**
 * Shutdown chiral resource management
 */
void chiral_resources_shutdown(void);

/**
 * Get chiral qubit pool state
 *
 * @param pool Output: current pool state
 * @return STATUS_SUCCESS or error
 */
status_t chiral_get_pool_state(chiral_pool_t *pool);

/* ============================================================================
 * Qubit Operations
 * ============================================================================ */

/**
 * Allocate chirally-enhanced qubits
 *
 * Allocates qubits meeting chiral stability requirements with
 * optional CISS enhancement and topological protection.
 *
 * @param request Allocation request parameters
 * @param result Output: allocation result
 * @return STATUS_SUCCESS or error
 */
status_t chiral_allocate(const chiral_alloc_request_t *request,
                         chiral_alloc_result_t *result);

/**
 * Deallocate chirally-enhanced qubits
 *
 * @param pid Process ID
 * @param qubit_ids Array of qubit IDs to deallocate
 * @param count Number of qubits
 * @return STATUS_SUCCESS or error
 */
status_t chiral_deallocate(uint32_t pid, const uint32_t *qubit_ids, uint32_t count);

/**
 * Get chiral qubit state
 *
 * @param qubit_id Qubit ID
 * @param qubit Output: chiral qubit state
 * @return STATUS_SUCCESS or error
 */
status_t chiral_get_qubit(uint32_t qubit_id, chiral_qubit_t *qubit);

/* ============================================================================
 * Stability Management
 * ============================================================================ */

/**
 * Optimize qubit chiral parameters
 *
 * Adjusts η and Γ toward optimal stability.
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_optimize_qubit(uint32_t qubit_id);

/**
 * Rebalance chirality for a qubit
 *
 * Attempts to restore stability for marginal/unstable qubits.
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_rebalance_qubit(uint32_t qubit_id);

/**
 * Flip qubit handedness
 *
 * Changes coupling direction for asymmetry rebalancing.
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_flip_handedness(uint32_t qubit_id);

/**
 * Get qubit stability classification
 *
 * @param qubit_id Qubit ID
 * @return Stability class
 */
chiral_stability_class_t chiral_get_stability(uint32_t qubit_id);

/**
 * Check if qubit is chirally stable
 *
 * @param qubit_id Qubit ID
 * @return true if |η/Γ| < 1.0
 */
bool chiral_is_stable(uint32_t qubit_id);

/* ============================================================================
 * CISS Enhancement
 * ============================================================================ */

/**
 * Enable CISS enhancement for a qubit
 *
 * Activates Chiral-Induced Spin Selectivity for coherence boost.
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_enable_ciss(uint32_t qubit_id);

/**
 * Disable CISS enhancement
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_disable_ciss(uint32_t qubit_id);

/**
 * Get CISS coherence boost factor
 *
 * @param qubit_id Qubit ID
 * @return Coherence boost factor (1.0 = no boost)
 */
double chiral_get_ciss_boost(uint32_t qubit_id);

/**
 * Get CISS-enhanced coherence time
 *
 * @param qubit_id Qubit ID
 * @return Enhanced coherence time in nanoseconds
 */
uint64_t chiral_get_enhanced_coherence(uint32_t qubit_id);

/* ============================================================================
 * Topological Protection
 * ============================================================================ */

/**
 * Enable topological protection for a qubit
 *
 * Creates topological charge for quantum error protection.
 *
 * @param qubit_id Qubit ID
 * @param target_charge Desired topological charge
 * @return STATUS_SUCCESS or error
 */
status_t chiral_enable_topological(uint32_t qubit_id, double target_charge);

/**
 * Disable topological protection
 *
 * @param qubit_id Qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_disable_topological(uint32_t qubit_id);

/**
 * Get topological charge
 *
 * @param qubit_id Qubit ID
 * @return Topological charge Q
 */
double chiral_get_topological_charge(uint32_t qubit_id);

/**
 * Check if qubit has topological protection
 *
 * @param qubit_id Qubit ID
 * @return true if topologically protected
 */
bool chiral_is_topologically_protected(uint32_t qubit_id);

/* ============================================================================
 * Coupling Operations
 * ============================================================================ */

/**
 * Create chiral coupling between qubits
 *
 * Establishes asymmetric coupling based on handedness.
 *
 * @param qubit1 First qubit ID
 * @param qubit2 Second qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_couple_qubits(uint32_t qubit1, uint32_t qubit2);

/**
 * Remove chiral coupling
 *
 * @param qubit1 First qubit ID
 * @param qubit2 Second qubit ID
 * @return STATUS_SUCCESS or error
 */
status_t chiral_decouple_qubits(uint32_t qubit1, uint32_t qubit2);

/**
 * Get coupling strength between qubits
 *
 * @param qubit1 First qubit ID
 * @param qubit2 Second qubit ID
 * @return Coupling strength (0.0 = no coupling)
 */
double chiral_get_coupling_strength(uint32_t qubit1, uint32_t qubit2);

/* ============================================================================
 * Pool Optimization
 * ============================================================================ */

/**
 * Optimize entire qubit pool
 *
 * Performs global optimization of chiral parameters.
 *
 * @return STATUS_SUCCESS or error
 */
status_t chiral_optimize_pool(void);

/**
 * Rebalance pool stability
 *
 * Attempts to restore stability across all qubits.
 *
 * @return Number of qubits rebalanced
 */
uint32_t chiral_rebalance_pool(void);

/**
 * Get pool stability metrics
 *
 * @param average_asymmetry Output: average |η/Γ|
 * @param stability_class Output: overall stability class
 * @return STATUS_SUCCESS or error
 */
status_t chiral_get_pool_stability(double *average_asymmetry,
                                   chiral_stability_class_t *stability_class);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * Dump chiral qubit state
 *
 * @param qubit_id Qubit ID (or -1 for all)
 */
void chiral_dump_qubit(int32_t qubit_id);

/**
 * Dump chiral pool state
 */
void chiral_dump_pool(void);

/**
 * Get stability statistics string
 *
 * @param buffer Output buffer
 * @param size Buffer size
 * @return Bytes written
 */
size_t chiral_get_stats_string(char *buffer, size_t size);

#endif /* CHIRAL_RESOURCES_H */
