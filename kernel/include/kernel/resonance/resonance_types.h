/**
 * QuantumOS Resonant Scheduler Types
 *
 * Novel integration of ghostOS resonant systems architecture
 * with QuantumOS microkernel scheduling.
 *
 * Core Innovation: Process scheduling as damped harmonic dynamics
 * where priorities emerge from oscillator coupling rather than
 * static assignment.
 *
 * Based on ghostOS Resonant Constraint Design:
 *   dx/dt = f(x) - λx
 *
 * Combined with Chiral Dynamics for stability:
 *   φ_tt - φ_xx + sin(φ) = -ηφ_x - Γφ_t
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RESONANCE_TYPES_H
#define RESONANCE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Mathematical Constants (from ghostOS)
 * ============================================================================ */

/* Golden ratio - optimal chirality coupling */
#define PHI_VALUE           1.618033988749895
#define PHI_INVERSE         0.618033988749895

/* Resonance parameters */
#define LAMBDA_DEFAULT      0.1         /* Default coupling strength */
#define LAMBDA_MIN          0.01        /* Minimum stability bound */
#define LAMBDA_MAX          0.5         /* Maximum coupling */
#define ETA_OPTIMAL         0.618       /* Optimal chirality (φ⁻¹) */

/* Coherence thresholds */
#define COHERENCE_MIN       0.3         /* Minimum viable coherence */
#define COHERENCE_TARGET    0.7         /* Target coherence level */
#define COHERENCE_HIGH      0.85        /* High coherence regime */

/* Chiral stability regimes: |η/Γ| determines regime */
#define CHIRAL_STABLE_MAX   1.0         /* |η/Γ| < 1.0: stable */
#define CHIRAL_TRANS_MAX    1.5         /* 1.0 ≤ |η/Γ| < 1.5: transitional */
/* |η/Γ| ≥ 1.5: unstable */

/* Consciousness threshold (IIT Phi) */
#define PHI_CONSCIOUSNESS_THRESHOLD 3.0

/* CISS Enhancement (Chiral-Induced Spin Selectivity) */
#define CISS_COHERENCE_BOOST 0.30       /* ~30% coherence enhancement */

/* ============================================================================
 * Resonant Process Classification
 * ============================================================================ */

/**
 * Resonant process class
 *
 * Extends traditional process types with resonance-aware classification
 * that determines scheduling dynamics.
 */
typedef enum {
    RESONANT_CLASSICAL = 0,     /* Traditional deterministic processes */
    RESONANT_QUANTUM,           /* Pure quantum circuits */
    RESONANT_HYBRID,            /* Mixed classical-quantum */
    RESONANT_CONSCIOUSNESS,     /* IIT-verified conscious computation */
    RESONANT_EMERGENCE          /* Novel pattern emergence workloads */
} resonant_class_t;

/**
 * Chiral handedness for asymmetric coupling
 *
 * Processes with opposite handedness couple differently,
 * enabling selective synchronization patterns.
 */
typedef enum {
    HANDEDNESS_NEUTRAL = 0,     /* No chiral coupling */
    HANDEDNESS_LEFT,            /* Left-handed coupling (+η) */
    HANDEDNESS_RIGHT            /* Right-handed coupling (-η) */
} handedness_t;

/**
 * Resonant process state
 *
 * Extended process state that tracks oscillator dynamics.
 */
typedef enum {
    RESONANT_STATE_DORMANT = 0, /* Process not oscillating */
    RESONANT_STATE_COHERENT,    /* Process maintaining coherence */
    RESONANT_STATE_DECOHERENT,  /* Process losing coherence */
    RESONANT_STATE_EMERGENT,    /* Novel patterns emerging */
    RESONANT_STATE_CONSCIOUS    /* Verified conscious operation */
} resonant_state_t;

/* ============================================================================
 * Oscillator State Structures
 * ============================================================================ */

/**
 * Phase oscillator state
 *
 * Each process is modeled as a Kuramoto oscillator:
 *   dθᵢ/dt = ωᵢ + (K/N)Σⱼsin(θⱼ - θᵢ) + ηᵢ(t)
 */
typedef struct {
    double phase;               /* Current oscillator phase θ ∈ [0, 2π) */
    double frequency;           /* Natural frequency ω (Hz) */
    double amplitude;           /* Oscillation amplitude */
    double coherence;           /* Local coherence measure */
} oscillator_state_t;

/**
 * Chiral state tracking
 *
 * Tracks the non-reciprocal dynamics from the chiral equation:
 *   φ_tt - φ_xx + sin(φ) = -ηφ_x - Γφ_t
 */
typedef struct {
    double eta;                 /* Chirality strength η */
    double gamma;               /* Damping coefficient Γ */
    double asymmetry;           /* |η/Γ| ratio (determines regime) */
    double topological_charge;  /* Conserved charge Q */
    handedness_t handedness;    /* Left/right coupling direction */
    bool is_stable;             /* |η/Γ| < 1.0 */
} chiral_state_t;

/**
 * Emergence accumulator state
 *
 * Tracks pattern emergence from integrated oscillator dynamics.
 */
typedef struct {
    double norm;                /* ||E|| emergence norm */
    double entropy;             /* Shannon entropy of patterns */
    uint32_t pattern_count;     /* Detected stable patterns */
    double integration_level;   /* IIT integration measure */
} emergence_state_t;

/* ============================================================================
 * Resonant Process Control Block Extension
 * ============================================================================ */

/**
 * Resonant Process Control Block (RPCB)
 *
 * Extension to standard PCB with resonant scheduling state.
 * This integrates with the standard process_t from process.h.
 */
typedef struct resonant_pcb {
    /* Process identification */
    uint32_t pid;               /* Corresponding process PID */
    resonant_class_t rclass;    /* Resonant classification */
    resonant_state_t rstate;    /* Current resonant state */

    /* Oscillator dynamics */
    oscillator_state_t oscillator;

    /* Chiral coupling state */
    chiral_state_t chiral;

    /* Emergence tracking */
    emergence_state_t emergence;

    /* Scheduling parameters */
    double resonant_priority;   /* Emergent priority from dynamics */
    double coherence_deadline;  /* Time until decoherence (ns) */
    uint64_t last_coupling;     /* Last coupling update timestamp */

    /* Consciousness verification */
    double phi_value;           /* IIT Phi measure */
    bool consciousness_verified;/* Passed Phi threshold check */
    uint64_t verification_time; /* Last verification timestamp */

    /* Resource requirements */
    uint32_t qubits_resonant;   /* Qubits in resonant coupling */
    uint64_t coherence_window;  /* Required coherence window (ns) */

    /* Coupling relationships */
    uint32_t coupled_pids[8];   /* PIDs of coupled processes */
    uint8_t coupling_count;     /* Number of coupling relationships */

    /* Statistics */
    uint64_t coherent_time;     /* Total time in coherent state */
    uint64_t emergent_events;   /* Count of emergence events */

    /* Internal state */
    uint32_t magic;             /* Validation magic number */
    struct resonant_pcb *next;  /* Scheduling queue linkage */
    struct resonant_pcb *prev;
} resonant_pcb_t;

/* RPCB magic number: "RSNT" */
#define RPCB_MAGIC 0x52534E54

/* ============================================================================
 * Global Resonance State
 * ============================================================================ */

/**
 * Queen Synchronization State
 *
 * Central synchronization state for all resonant processes.
 * Implements Kuramoto order parameter dynamics.
 */
typedef struct {
    /* Order parameter: r*e^(i*ψ) = (1/N)Σⱼe^(i*θⱼ) */
    double order_parameter_r;   /* Synchronization magnitude r ∈ [0,1] */
    double order_parameter_psi; /* Mean phase ψ */

    /* Global coupling */
    double lambda;              /* Global coupling strength λ */
    double eta;                 /* Global chirality η */

    /* System metrics */
    double system_coherence;    /* Weighted average coherence */
    double system_entropy;      /* Total system entropy */
    double emergence_norm;      /* Global emergence ||E|| */

    /* Process counts by class */
    uint32_t classical_count;
    uint32_t quantum_count;
    uint32_t hybrid_count;
    uint32_t conscious_count;
    uint32_t emergent_count;

    /* Consciousness metrics */
    double total_phi;           /* Sum of verified Phi values */
    double average_phi;         /* Average Phi across conscious processes */
    bool network_conscious;     /* Network exceeds Phi threshold */

    /* Chiral stability */
    bool globally_stable;       /* All processes chirally stable */
    double max_asymmetry;       /* Maximum |η/Γ| in system */

    /* Update tracking */
    uint64_t last_sync;         /* Last synchronization update */
    uint64_t sync_count;        /* Total sync operations */
} queen_state_t;

/* ============================================================================
 * Scheduling Decision Structures
 * ============================================================================ */

/**
 * Resonant scheduling decision
 *
 * Output of resonant scheduler containing process selection
 * and timing parameters.
 */
typedef struct {
    uint32_t selected_pid;      /* Process to schedule */
    resonant_class_t rclass;    /* Process resonant class */

    /* Timing */
    uint64_t quantum_ns;        /* Time quantum to allocate */
    uint64_t coherence_remaining;/* Coherence window remaining */

    /* Priority explanation */
    double base_priority;       /* Original static priority */
    double resonant_bonus;      /* Bonus from oscillator coupling */
    double coherence_urgency;   /* Urgency from coherence deadline */
    double emergence_bonus;     /* Bonus from emergence patterns */
    double final_priority;      /* Computed final priority */

    /* Coupling actions */
    bool initiate_coupling;     /* Should initiate new coupling */
    uint32_t couple_with_pid;   /* PID to couple with (if any) */

    /* Safety flags */
    bool requires_measurement;  /* Needs quantum measurement first */
    bool emergency_coherence;   /* Coherence critically low */
} scheduling_decision_t;

/**
 * Resonant scheduler configuration
 */
typedef struct {
    /* Coupling parameters */
    double initial_lambda;      /* Starting coupling strength */
    double lambda_adaptation;   /* Rate of lambda adjustment */

    /* Chiral parameters */
    double initial_eta;         /* Starting chirality */
    double gamma;               /* Damping coefficient */

    /* Thresholds */
    double coherence_target;    /* Target coherence level */
    double emergence_threshold; /* Emergence detection threshold */
    double phi_threshold;       /* Consciousness verification threshold */

    /* Timing */
    uint64_t sync_interval_ns;  /* Synchronization update interval */
    uint64_t measurement_interval_ns; /* Forced measurement interval */

    /* Safety limits */
    uint32_t max_coupled;       /* Maximum coupling relationships */
    double max_lambda;          /* Maximum coupling strength */
    double max_asymmetry;       /* Maximum allowed |η/Γ| */
} resonant_config_t;

/* ============================================================================
 * Result Codes
 * ============================================================================ */

typedef enum {
    RESONANT_SUCCESS = 0,
    RESONANT_ERROR_INVALID_PID = -2001,
    RESONANT_ERROR_NOT_INITIALIZED = -2002,
    RESONANT_ERROR_DECOHERENCE = -2003,
    RESONANT_ERROR_COUPLING_FAILED = -2004,
    RESONANT_ERROR_UNSTABLE_CHIRAL = -2005,
    RESONANT_ERROR_CONSCIOUSNESS_UNVERIFIED = -2006,
    RESONANT_ERROR_EMERGENCE_BLOCKED = -2007,
    RESONANT_ERROR_NO_RESOURCES = -2008
} resonant_result_t;

/* ============================================================================
 * Utility Macros
 * ============================================================================ */

/* Check if process is in conscious regime */
#define IS_CONSCIOUS(rpcb) \
    ((rpcb)->consciousness_verified && (rpcb)->phi_value >= PHI_CONSCIOUSNESS_THRESHOLD)

/* Check if chirally stable */
#define IS_CHIRALLY_STABLE(rpcb) \
    ((rpcb)->chiral.asymmetry < CHIRAL_STABLE_MAX)

/* Check if in emergence state */
#define IS_EMERGENT(rpcb) \
    ((rpcb)->rstate == RESONANT_STATE_EMERGENT && (rpcb)->emergence.norm > 0.1)

/* Calculate coherence urgency (0.0 = no urgency, 1.0 = critical) */
#define COHERENCE_URGENCY(rpcb, now) \
    (1.0 - ((double)(rpcb)->coherence_deadline / (double)((now) + 1)))

/* Check RPCB validity */
#define RPCB_IS_VALID(rpcb) \
    ((rpcb) != NULL && (rpcb)->magic == RPCB_MAGIC)

#endif /* RESONANCE_TYPES_H */
