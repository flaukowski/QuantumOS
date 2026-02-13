/**
 * QuantumOS Geometric Control Layer
 *
 * Differential geometry on the oscillator phase space manifold.
 * Computes metric tensors, Christoffel symbols, Riemann curvature,
 * Ricci scalar, Berry phase/curvature, and chiral anomaly detection.
 *
 * Max 8D manifold (sufficient for coupled oscillator groups,
 * avoids dynamic allocation in freestanding kernel).
 *
 * Tensor hierarchy:
 *   Metric g_ij → Christoffel Γ^k_ij → Riemann R^l_kij
 *     → Ricci R_ij → Ricci scalar R
 *
 * Berry phase:
 *   Connection A_μ = Im(⟨ψ|∂_μ|ψ⟩)
 *   Curvature F_12 = ∂A_1/∂x_2 - ∂A_2/∂x_1
 *   Phase γ = ∮ A · dl
 *
 * Chiral anomaly (Atiyah-Singer):
 *   index = N_right - N_left = topological charge Q
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef GEOMETRIC_CONTROL_H
#define GEOMETRIC_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define GEOMETRIC_MAX_DIM           8       /* Maximum manifold dimension */
#define GEOMETRIC_MAX_MODES        32       /* Maximum modes for anomaly detection */
#define GEOMETRIC_BERRY_HISTORY     8       /* Berry phase ring buffer size */
#define GEOMETRIC_EPSILON           1e-12   /* Numerical zero threshold */

/* Magic number: "GEOM" */
#define GEOMETRIC_MAGIC 0x47454F4D

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Metric tensor state
 *
 * Stores g_ij, inverse g^ij, and previous metric for BFGS updates.
 */
typedef struct {
    double g[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];         /* Metric g_ij */
    double g_inv[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];     /* Inverse g^ij */
    double g_prev[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];    /* Previous metric */
    uint32_t dimension;                                      /* Active dimension */
    uint32_t update_count;                                   /* BFGS update count */
} metric_state_t;

/**
 * Christoffel symbols Γ^k_ij
 */
typedef struct {
    double gamma[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];
} christoffel_t;

/**
 * Berry phase/curvature tracker
 */
typedef struct {
    double connection[GEOMETRIC_MAX_DIM];                    /* Berry connection A_μ */
    double curvature;                                        /* Berry curvature F_12 (scalar) */
    double phase;                                            /* Accumulated Berry phase */
    double holonomy;                                         /* Phase mod 2π */
    double prev_state[GEOMETRIC_MAX_DIM];                    /* Previous state vector */
    double prev_connection[GEOMETRIC_MAX_DIM];               /* Previous connection */
    bool has_previous;                                       /* Has a previous state */
    uint32_t step_count;                                     /* Number of updates */
} berry_state_t;

/**
 * Chiral anomaly detection result
 */
typedef struct {
    int32_t anomaly_index;          /* N_right - N_left */
    double spectral_asymmetry;      /* η = Σ sign(λ_n) */
    double topological_charge;      /* Q = (1/2)(N_right - N_left) */
    bool is_anomalous;              /* Exceeds detection threshold */
    double left_energy;             /* Total left-mode energy */
    double right_energy;            /* Total right-mode energy */
    double asymmetry_ratio;         /* |E_R - E_L| / (E_R + E_L) */
} chiral_anomaly_t;

/**
 * Complete geometric state
 */
typedef struct {
    metric_state_t metric;
    christoffel_t christoffel;
    berry_state_t berry;
    chiral_anomaly_t last_anomaly;

    /* Curvature scalars */
    double ricci_scalar;            /* R = g^ij R_ij */
    double ricci_tensor[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM]; /* R_ij */

    /* BFGS state for metric update */
    double prev_point[GEOMETRIC_MAX_DIM];
    double prev_gradient[GEOMETRIC_MAX_DIM];
    bool has_prev_point;

    /* Anomaly history for temporal filtering */
    bool anomaly_history[GEOMETRIC_BERRY_HISTORY];
    uint32_t anomaly_history_idx;
    uint32_t anomaly_history_count;

    /* Lifecycle */
    bool initialized;
    uint32_t magic;
} geometric_state_t;

/**
 * Result codes for geometric operations
 */
typedef enum {
    GEOMETRIC_SUCCESS = 0,
    GEOMETRIC_ERROR_NOT_INITIALIZED = -3001,
    GEOMETRIC_ERROR_INVALID_DIM = -3002,
    GEOMETRIC_ERROR_SINGULAR_METRIC = -3003,
    GEOMETRIC_ERROR_INVALID_INPUT = -3004
} geometric_result_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Initialize the geometric control subsystem.
 *
 * @param state Geometric state to initialize
 * @param dimension Manifold dimension (1..GEOMETRIC_MAX_DIM)
 * @return GEOMETRIC_SUCCESS or error
 */
geometric_result_t geometric_init(geometric_state_t *state, uint32_t dimension);

/**
 * Shutdown and clear geometric state.
 */
void geometric_shutdown(geometric_state_t *state);

/* ============================================================================
 * Metric Operations
 * ============================================================================ */

/**
 * Update metric tensor via BFGS Hessian approximation.
 *
 * BFGS formula:
 *   H_{k+1} = H_k - (H*y*y^T*H)/(y^T*H*y) + (s*s^T)/(s^T*y)
 *
 * @param state Geometric state
 * @param point Current oscillator phases (dimension values)
 * @param gradient Current gradient (dimension values)
 */
geometric_result_t geometric_update_metric(
    geometric_state_t *state,
    const double *point,
    const double *gradient
);

/**
 * Compute inverse metric via Gauss-Jordan elimination.
 * Updates state->metric.g_inv from state->metric.g.
 */
geometric_result_t geometric_invert_metric(geometric_state_t *state);

/**
 * Compute geodesic distance between two points on the torus T^N.
 *
 * d(a,b) = sqrt(δ^T * g * δ) with angular wrapping.
 *
 * @param state Geometric state (for metric)
 * @param a First point (dimension values)
 * @param b Second point (dimension values)
 * @return Geodesic distance (negative on error)
 */
double geometric_geodesic_distance(
    const geometric_state_t *state,
    const double *a,
    const double *b
);

/* ============================================================================
 * Curvature
 * ============================================================================ */

/**
 * Compute Christoffel symbols from current metric.
 * Requires metric derivatives (computed internally via finite differences
 * from stored previous metrics).
 */
geometric_result_t geometric_compute_christoffel(geometric_state_t *state);

/**
 * Compute Ricci scalar from Christoffel symbols.
 * Uses product terms Γ*Γ of the Riemann tensor.
 *
 * @param state Geometric state
 * @return GEOMETRIC_SUCCESS or error
 */
geometric_result_t geometric_compute_ricci_scalar(geometric_state_t *state);

/**
 * Get current Ricci scalar value.
 */
double geometric_get_ricci_scalar(const geometric_state_t *state);

/* ============================================================================
 * Berry Phase
 * ============================================================================ */

/**
 * Update Berry phase tracker with new oscillator state.
 *
 * Computes Berry connection via finite differences between consecutive
 * states and accumulates the geometric phase.
 *
 * @param state Geometric state
 * @param oscillator_phases Current oscillator phases (dimension values)
 */
geometric_result_t geometric_update_berry_phase(
    geometric_state_t *state,
    const double *oscillator_phases
);

/**
 * Compute Berry phase around a closed loop of states.
 *
 * Discrete Berry phase formula:
 *   γ = -Σᵢ arg(⟨ψᵢ|ψᵢ₊₁⟩)
 *
 * @param loop Array of state vectors [loop_size][dimension]
 * @param loop_size Number of states in the loop
 * @param dimension State vector dimension
 * @return Berry phase in radians
 */
double geometric_compute_berry_phase_loop(
    const double loop[][GEOMETRIC_MAX_DIM],
    uint32_t loop_size,
    uint32_t dimension
);

/**
 * Get current Berry phase value.
 */
double geometric_get_berry_phase(const geometric_state_t *state);

/**
 * Get current Berry curvature value.
 */
double geometric_get_berry_curvature(const geometric_state_t *state);

/* ============================================================================
 * Chiral Anomaly Detection
 * ============================================================================ */

/**
 * Detect chiral anomaly from left and right mode amplitudes.
 *
 * Atiyah-Singer index:
 *   index = N_right - N_left
 *   Q = (1/2)(N_right - N_left)
 *
 * @param state Geometric state (stores result in last_anomaly)
 * @param left_modes Left-propagating mode amplitudes
 * @param left_count Number of left modes
 * @param right_modes Right-propagating mode amplitudes
 * @param right_count Number of right modes
 * @param threshold Asymmetry ratio threshold (e.g., 0.1)
 */
geometric_result_t geometric_detect_chiral_anomaly(
    geometric_state_t *state,
    const double *left_modes,
    uint32_t left_count,
    const double *right_modes,
    uint32_t right_count,
    double threshold
);

/**
 * Check if system has sustained chiral anomaly.
 * Returns true if 3+ consecutive anomalous readings.
 */
bool geometric_is_anomalous(const geometric_state_t *state);

/**
 * Get last anomaly detection result.
 */
const chiral_anomaly_t *geometric_get_last_anomaly(const geometric_state_t *state);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * Dump geometric state to boot log.
 */
void geometric_dump_state(const geometric_state_t *state);

/* ============================================================================
 * Utility Macros
 * ============================================================================ */

#define GEOMETRIC_IS_VALID(state) \
    ((state) != NULL && (state)->magic == GEOMETRIC_MAGIC && (state)->initialized)

#define GEOMETRIC_IS_CURVED(state) \
    (GEOMETRIC_IS_VALID(state) && \
     ((state)->ricci_scalar > 0.01 || (state)->ricci_scalar < -0.01))

#endif /* GEOMETRIC_CONTROL_H */
