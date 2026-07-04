/**
 * QuantumOS Geometric Control Layer Implementation
 *
 * Differential geometry on the oscillator phase space manifold.
 * Implements metric tensor management, curvature computation,
 * Berry phase tracking, and chiral anomaly detection.
 *
 * Corresponds to the TypeScript geometric module in ghostOS:
 *   ghostOS/src/geometric/{manifold,berry,curvature,anomaly}.ts
 *
 * Uses the same fast_sin/fast_cos/fast_sqrt helpers from
 * resonant_scheduler.c (freestanding kernel, no libm).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/resonance/geometric_control.h>
#include <kernel/boot.h>

/* ============================================================================
 * Mathematical Helpers (mirrors resonant_scheduler.c)
 * ============================================================================ */

#define PI 3.14159265358979323846
#define TWO_PI (2.0 * PI)

static double fast_sin(double x) {
    while (x > PI)
        x -= TWO_PI;
    while (x < -PI)
        x += TWO_PI;
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    double x7 = x5 * x2;
    return x - x3 / 6.0 + x5 / 120.0 - x7 / 5040.0;
}

static double fast_cos(double x) {
    return fast_sin(x + PI / 2.0);
}

static double fast_sqrt(double x) {
    if (x <= 0)
        return 0;
    double guess = x / 2.0;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) / 2.0;
    }
    return guess;
}

static double fast_asin(double x) {
    if (x > 1.0)
        x = 1.0;
    if (x < -1.0)
        x = -1.0;
    double x2 = x * x;
    return x * (1.0 + x2 * (1.0 / 6.0 + x2 * (3.0 / 40.0 + x2 * 15.0 / 336.0)));
}

static double fast_atan2(double y, double x) {
    double r = fast_sqrt(x * x + y * y);
    if (r < 1e-15)
        return 0;
    double sin_val = y / r;
    if (x > 0)
        return fast_asin(sin_val);
    if (x < 0 && y >= 0)
        return PI - fast_asin(sin_val);
    if (x < 0 && y < 0)
        return -PI - fast_asin(sin_val);
    if (y > 0)
        return PI / 2.0;
    if (y < 0)
        return -PI / 2.0;
    return 0;
}

static double fast_abs(double x) {
    return x < 0 ? -x : x;
}

static double clamp(double value, double min, double max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/* Simple memset for doubles */
static void zero_doubles(double *ptr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        ptr[i] = 0.0;
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

geometric_result_t geometric_init(geometric_state_t *state, uint32_t dimension) {
    if (!state) {
        return GEOMETRIC_ERROR_INVALID_INPUT;
    }
    if (dimension == 0 || dimension > GEOMETRIC_MAX_DIM) {
        return GEOMETRIC_ERROR_INVALID_DIM;
    }

    /* Zero entire state */
    uint8_t *bytes = (uint8_t *)state;
    for (uint32_t i = 0; i < sizeof(geometric_state_t); i++) {
        bytes[i] = 0;
    }

    state->metric.dimension = dimension;
    state->metric.update_count = 0;

    /* Initialize metric as identity */
    for (uint32_t i = 0; i < dimension; i++) {
        state->metric.g[i][i] = 1.0;
        state->metric.g_inv[i][i] = 1.0;
        state->metric.g_prev[i][i] = 1.0;
    }

    /* Berry tracker */
    state->berry.has_previous = false;
    state->berry.phase = 0.0;
    state->berry.curvature = 0.0;
    state->berry.holonomy = 0.0;
    state->berry.step_count = 0;

    /* BFGS */
    state->has_prev_point = false;

    /* Anomaly history */
    state->anomaly_history_idx = 0;
    state->anomaly_history_count = 0;

    /* Lifecycle */
    state->initialized = true;
    state->magic = GEOMETRIC_MAGIC;

    boot_log("Geometric control initialized");

    return GEOMETRIC_SUCCESS;
}

void geometric_shutdown(geometric_state_t *state) {
    if (!state)
        return;

    state->initialized = false;
    state->magic = 0;

    boot_log("Geometric control shutdown");
}

/* ============================================================================
 * Metric Operations
 * ============================================================================ */

geometric_result_t geometric_update_metric(geometric_state_t *state, const double *point,
                                           const double *gradient) {
    if (!GEOMETRIC_IS_VALID(state) || !point || !gradient) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }

    uint32_t n = state->metric.dimension;

    /* Save previous metric */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            state->metric.g_prev[i][j] = state->metric.g[i][j];
        }
    }

    /* Need previous point for BFGS update */
    if (!state->has_prev_point) {
        for (uint32_t i = 0; i < n; i++) {
            state->prev_point[i] = point[i];
            state->prev_gradient[i] = gradient[i];
        }
        state->has_prev_point = true;
        state->metric.update_count++;
        return GEOMETRIC_SUCCESS;
    }

    /* Compute s = new_pos - old_pos (wrapped on torus) */
    double s[GEOMETRIC_MAX_DIM];
    for (uint32_t i = 0; i < n; i++) {
        double diff = point[i] - state->prev_point[i];
        while (diff > PI)
            diff -= TWO_PI;
        while (diff < -PI)
            diff += TWO_PI;
        s[i] = diff;
    }

    /* Compute y = new_grad - old_grad */
    double y[GEOMETRIC_MAX_DIM];
    for (uint32_t i = 0; i < n; i++) {
        y[i] = gradient[i] - state->prev_gradient[i];
    }

    /* Compute s^T y (curvature condition) */
    double sTy = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        sTy += s[i] * y[i];
    }

    /* Only update if curvature condition satisfied (positive definite guarantee) */
    if (sTy > 1e-10) {
        double rho = 1.0 / sTy;

        /* Compute H * y */
        double Hy[GEOMETRIC_MAX_DIM];
        zero_doubles(Hy, GEOMETRIC_MAX_DIM);
        for (uint32_t i = 0; i < n; i++) {
            double sum = 0.0;
            for (uint32_t j = 0; j < n; j++) {
                sum += state->metric.g[i][j] * y[j];
            }
            Hy[i] = sum;
        }

        /* Compute y^T H y */
        double yTHy = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            yTHy += y[i] * Hy[i];
        }

        /* Full BFGS rank-2 inverse Hessian update:
         *   H_new = (I - ρ·s·yᵀ)·H·(I - ρ·y·sᵀ) + ρ·s·sᵀ
         * Expanded: H - ρ·s·(Hy)ᵀ - ρ·(Hy)·sᵀ + ρ·(1 + ρ·yᵀHy)·s·sᵀ */
        double factor = 1.0 + rho * yTHy;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                state->metric.g[i][j] +=
                    rho * factor * s[i] * s[j] - rho * (s[i] * Hy[j] + Hy[i] * s[j]);
            }
        }

        /* Ensure symmetry */
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                double avg = 0.5 * (state->metric.g[i][j] + state->metric.g[j][i]);
                state->metric.g[i][j] = avg;
                state->metric.g[j][i] = avg;
            }
        }

        /* Ensure positive definiteness: clamp diagonal */
        for (uint32_t i = 0; i < n; i++) {
            if (state->metric.g[i][i] < 1e-6) {
                state->metric.g[i][i] = 1e-6;
            }
        }

        /* Track Berry phase from metric evolution */
        double metric_berry = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            double old_diag = state->metric.g_prev[i][i];
            if (fast_abs(old_diag) > 1e-15) {
                metric_berry += (state->metric.g[i][i] - old_diag) / old_diag;
            }
        }
        state->berry.phase += metric_berry / (double)n;
    }

    /* Store current state for next update */
    for (uint32_t i = 0; i < n; i++) {
        state->prev_point[i] = point[i];
        state->prev_gradient[i] = gradient[i];
    }
    state->metric.update_count++;

    /* Update inverse metric */
    geometric_invert_metric(state);

    return GEOMETRIC_SUCCESS;
}

geometric_result_t geometric_invert_metric(geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state)) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }

    uint32_t n = state->metric.dimension;

    /* Build augmented matrix [A | I] in local storage */
    double aug[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM * 2];
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            aug[i][j] = state->metric.g[i][j];
        }
        for (uint32_t j = 0; j < n; j++) {
            aug[i][n + j] = (i == j) ? 1.0 : 0.0;
        }
    }

    /* Gauss-Jordan with partial pivoting */
    for (uint32_t col = 0; col < n; col++) {
        /* Find pivot */
        double max_val = fast_abs(aug[col][col]);
        uint32_t max_row = col;
        for (uint32_t row = col + 1; row < n; row++) {
            double val = fast_abs(aug[row][col]);
            if (val > max_val) {
                max_val = val;
                max_row = row;
            }
        }

        /* Swap rows if needed */
        if (max_row != col) {
            for (uint32_t j = 0; j < 2 * n; j++) {
                double tmp = aug[col][j];
                aug[col][j] = aug[max_row][j];
                aug[max_row][j] = tmp;
            }
        }

        /* Check for singular */
        double pivot = aug[col][col];
        if (fast_abs(pivot) < 1e-12) {
            /* Singular: set inverse to identity */
            for (uint32_t i = 0; i < n; i++) {
                for (uint32_t j = 0; j < n; j++) {
                    state->metric.g_inv[i][j] = (i == j) ? 1.0 : 0.0;
                }
            }
            return GEOMETRIC_ERROR_SINGULAR_METRIC;
        }

        /* Scale pivot row */
        double inv_pivot = 1.0 / pivot;
        for (uint32_t j = 0; j < 2 * n; j++) {
            aug[col][j] *= inv_pivot;
        }

        /* Eliminate column in other rows */
        for (uint32_t row = 0; row < n; row++) {
            if (row == col)
                continue;
            double factor = aug[row][col];
            for (uint32_t j = 0; j < 2 * n; j++) {
                aug[row][j] -= factor * aug[col][j];
            }
        }
    }

    /* Extract inverse */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            state->metric.g_inv[i][j] = aug[i][n + j];
        }
    }

    return GEOMETRIC_SUCCESS;
}

double geometric_geodesic_distance(const geometric_state_t *state, const double *a,
                                   const double *b) {
    if (!GEOMETRIC_IS_VALID(state) || !a || !b) {
        return -1.0;
    }

    uint32_t n = state->metric.dimension;
    double delta[GEOMETRIC_MAX_DIM];

    /* Compute wrapped angular distances */
    for (uint32_t i = 0; i < n; i++) {
        double diff = fast_abs(a[i] - b[i]);
        if (diff > PI) {
            diff = TWO_PI - diff;
        }
        delta[i] = diff;
    }

    /* Compute δ^T * g * δ */
    double dist_sq = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            dist_sq += delta[i] * state->metric.g[i][j] * delta[j];
        }
    }

    if (dist_sq < 0.0)
        dist_sq = 0.0;
    return fast_sqrt(dist_sq);
}

/* ============================================================================
 * Curvature Computation
 * ============================================================================ */

/**
 * Compute Christoffel symbols from metric and its derivatives.
 *
 * We approximate metric derivatives using the difference between
 * current and previous metric states, scaled by the BFGS step.
 * For a more accurate result, we use the product-term-only
 * Riemann tensor computation (Γ*Γ terms) which doesn't require
 * explicit Christoffel derivatives.
 */
geometric_result_t geometric_compute_christoffel(geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state)) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }

    uint32_t n = state->metric.dimension;

    /* Approximate metric derivatives from current vs previous metric
     * d_k g_ij ≈ (g_ij - g_prev_ij) * normalization
     *
     * Since we don't have true directional derivatives, we use the
     * metric difference as an approximation. This gives first-order
     * accuracy for slowly-varying metrics. */
    double dg[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];

    /* Zero all derivatives */
    for (uint32_t k = 0; k < n; k++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                dg[k][i][j] = 0.0;
            }
        }
    }

    /* Approximate: distribute metric change across all directions */
    if (state->metric.update_count > 1) {
        double inv_n = 1.0 / (double)n;
        for (uint32_t k = 0; k < n; k++) {
            for (uint32_t i = 0; i < n; i++) {
                for (uint32_t j = 0; j < n; j++) {
                    dg[k][i][j] = (state->metric.g[i][j] - state->metric.g_prev[i][j]) * inv_n;
                }
            }
        }
    }

    /* Γ^k_ij = (1/2) g^kl (∂_i g_jl + ∂_j g_il - ∂_l g_ij) */
    for (uint32_t k = 0; k < n; k++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i; j < n; j++) {
                double sum = 0.0;
                for (uint32_t l = 0; l < n; l++) {
                    double di_gjl = dg[i][j][l];
                    double dj_gil = dg[j][i][l];
                    double dl_gij = dg[l][i][j];
                    sum += state->metric.g_inv[k][l] * (di_gjl + dj_gil - dl_gij);
                }
                double value = 0.5 * sum;
                state->christoffel.gamma[k][i][j] = value;
                state->christoffel.gamma[k][j][i] = value; /* Symmetric in lower indices */
            }
        }
    }

    return GEOMETRIC_SUCCESS;
}

geometric_result_t geometric_compute_ricci_scalar(geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state)) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }

    /* First compute Christoffel symbols */
    geometric_result_t res = geometric_compute_christoffel(state);
    if (res != GEOMETRIC_SUCCESS && res != GEOMETRIC_ERROR_SINGULAR_METRIC) {
        return res;
    }

    uint32_t n = state->metric.dimension;

    /* Compute Riemann tensor using product terms only:
     * R^l_kij ≈ Γ^l_im Γ^m_jk - Γ^l_jm Γ^m_ik
     *
     * The derivative terms (∂_i Γ - ∂_j Γ) would require Christoffel
     * symbols at neighboring points. For the kernel's purposes, the
     * product terms capture the leading-order curvature. */
    double riemann[GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM][GEOMETRIC_MAX_DIM];

    /* Zero */
    for (uint32_t l = 0; l < n; l++)
        for (uint32_t k = 0; k < n; k++)
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t j = 0; j < n; j++)
                    riemann[l][k][i][j] = 0.0;

    /* Product terms */
    for (uint32_t l = 0; l < n; l++) {
        for (uint32_t k = 0; k < n; k++) {
            for (uint32_t i = 0; i < n; i++) {
                for (uint32_t j = 0; j < n; j++) {
                    double value = 0.0;
                    for (uint32_t m = 0; m < n; m++) {
                        value +=
                            state->christoffel.gamma[l][i][m] * state->christoffel.gamma[m][j][k];
                        value -=
                            state->christoffel.gamma[l][j][m] * state->christoffel.gamma[m][i][k];
                    }
                    riemann[l][k][i][j] = value;
                }
            }
        }
    }

    /* Contract to Ricci tensor: R_ij = R^k_ikj */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            double sum = 0.0;
            for (uint32_t k = 0; k < n; k++) {
                sum += riemann[k][i][k][j];
            }
            state->ricci_tensor[i][j] = sum;
        }
    }

    /* Contract to Ricci scalar: R = g^ij R_ij */
    double scalar = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            scalar += state->metric.g_inv[i][j] * state->ricci_tensor[i][j];
        }
    }
    state->ricci_scalar = scalar;

    return GEOMETRIC_SUCCESS;
}

double geometric_get_ricci_scalar(const geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state))
        return 0.0;
    return state->ricci_scalar;
}

/* ============================================================================
 * Berry Phase
 * ============================================================================ */

geometric_result_t geometric_update_berry_phase(geometric_state_t *state,
                                                const double *oscillator_phases) {
    if (!GEOMETRIC_IS_VALID(state) || !oscillator_phases) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }

    uint32_t n = state->metric.dimension;

    if (!state->berry.has_previous) {
        /* Store first state */
        for (uint32_t i = 0; i < n; i++) {
            state->berry.prev_state[i] = oscillator_phases[i];
        }
        state->berry.has_previous = true;
        state->berry.step_count = 1;
        return GEOMETRIC_SUCCESS;
    }

    /* Compute overlap between consecutive states on the torus */
    /* For oscillator phases θ_i, the "state vector" is (cos θ_i, sin θ_i)
     * and the overlap captures the angular transport. */
    double dot = 0.0;
    double norm_prev = 0.0;
    double norm_curr = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        /* Treat phases as unit vectors for overlap computation */
        double prev_cos = fast_cos(state->berry.prev_state[i]);
        double prev_sin = fast_sin(state->berry.prev_state[i]);
        double curr_cos = fast_cos(oscillator_phases[i]);
        double curr_sin = fast_sin(oscillator_phases[i]);

        dot += prev_cos * curr_cos + prev_sin * curr_sin;
        norm_prev += prev_cos * prev_cos + prev_sin * prev_sin;
        norm_curr += curr_cos * curr_cos + curr_sin * curr_sin;
    }

    norm_prev = fast_sqrt(norm_prev);
    norm_curr = fast_sqrt(norm_curr);

    if (norm_prev > GEOMETRIC_EPSILON && norm_curr > GEOMETRIC_EPSILON) {
        double normalized_dot = dot / (norm_prev * norm_curr);
        normalized_dot = clamp(normalized_dot, -1.0, 1.0);

        /* Cross product magnitude for sign */
        double signed_cross = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            double prev_cos = fast_cos(state->berry.prev_state[i]) / norm_prev;
            double prev_sin = fast_sin(state->berry.prev_state[i]) / norm_prev;
            double curr_cos = fast_cos(oscillator_phases[i]) / norm_curr;
            double curr_sin = fast_sin(oscillator_phases[i]) / norm_curr;
            signed_cross += prev_cos * curr_sin - prev_sin * curr_cos;
        }

        /* Compute overlap angle */
        double cross_mag_sq = 1.0 - normalized_dot * normalized_dot;
        if (cross_mag_sq < 0.0)
            cross_mag_sq = 0.0;
        double cross_mag = fast_sqrt(cross_mag_sq);
        double sign = signed_cross >= 0 ? 1.0 : -1.0;
        double phi = fast_atan2(sign * cross_mag, normalized_dot);

        state->berry.phase += phi;

        /* Update connection: A_i ≈ phi / delta_theta_i */
        for (uint32_t i = 0; i < n; i++) {
            double delta = oscillator_phases[i] - state->berry.prev_state[i];
            while (delta > PI)
                delta -= TWO_PI;
            while (delta < -PI)
                delta += TWO_PI;
            if (fast_abs(delta) > GEOMETRIC_EPSILON) {
                double new_conn = phi / delta;
                /* Estimate curvature from connection change */
                if (state->berry.step_count > 1 && fast_abs(delta) > GEOMETRIC_EPSILON) {
                    state->berry.curvature += (new_conn - state->berry.prev_connection[i]) / delta;
                }
                state->berry.prev_connection[i] = new_conn;
                state->berry.connection[i] = new_conn;
            }
        }

        /* Normalize curvature by dimension */
        if (n > 0) {
            state->berry.curvature /= (double)n;
        }
    }

    /* Update holonomy: phase mod 2π in [-π, π] */
    double holonomy = state->berry.phase;
    while (holonomy > PI)
        holonomy -= TWO_PI;
    while (holonomy < -PI)
        holonomy += TWO_PI;
    state->berry.holonomy = holonomy;

    /* Store current state */
    for (uint32_t i = 0; i < n; i++) {
        state->berry.prev_state[i] = oscillator_phases[i];
    }
    state->berry.step_count++;

    return GEOMETRIC_SUCCESS;
}

double geometric_compute_berry_phase_loop(const double loop[][GEOMETRIC_MAX_DIM],
                                          uint32_t loop_size, uint32_t dimension) {
    if (!loop || loop_size < 3 || dimension == 0 || dimension > GEOMETRIC_MAX_DIM) {
        return 0.0;
    }

    double total_phase = 0.0;

    for (uint32_t k = 0; k < loop_size; k++) {
        const double *current = loop[k];
        const double *next = loop[(k + 1) % loop_size];

        /* Compute overlap using cos/sin embedding */
        double dot = 0.0;
        double norm_curr = 0.0;
        double norm_next = 0.0;

        for (uint32_t i = 0; i < dimension; i++) {
            double cc = fast_cos(current[i]);
            double cs = fast_sin(current[i]);
            double nc = fast_cos(next[i]);
            double ns = fast_sin(next[i]);

            dot += cc * nc + cs * ns;
            norm_curr += cc * cc + cs * cs;
            norm_next += nc * nc + ns * ns;
        }

        norm_curr = fast_sqrt(norm_curr);
        norm_next = fast_sqrt(norm_next);

        if (norm_curr < GEOMETRIC_EPSILON || norm_next < GEOMETRIC_EPSILON) {
            continue;
        }

        double normalized_dot = dot / (norm_curr * norm_next);
        normalized_dot = clamp(normalized_dot, -1.0, 1.0);

        /* Signed cross product */
        double signed_cross = 0.0;
        for (uint32_t i = 0; i < dimension; i++) {
            double cc = fast_cos(current[i]) / norm_curr;
            double cs = fast_sin(current[i]) / norm_curr;
            double nc = fast_cos(next[i]) / norm_next;
            double ns = fast_sin(next[i]) / norm_next;
            signed_cross += cc * ns - cs * nc;
        }

        double cross_mag_sq = 1.0 - normalized_dot * normalized_dot;
        if (cross_mag_sq < 0.0)
            cross_mag_sq = 0.0;
        double cross_mag = fast_sqrt(cross_mag_sq);
        double sign = signed_cross >= 0 ? 1.0 : -1.0;

        total_phase += fast_atan2(sign * cross_mag, normalized_dot);
    }

    return total_phase;
}

double geometric_get_berry_phase(const geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state))
        return 0.0;
    return state->berry.phase;
}

double geometric_get_berry_curvature(const geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state))
        return 0.0;
    return state->berry.curvature;
}

/* ============================================================================
 * Chiral Anomaly Detection
 * ============================================================================ */

geometric_result_t geometric_detect_chiral_anomaly(geometric_state_t *state,
                                                   const double *left_modes, uint32_t left_count,
                                                   const double *right_modes, uint32_t right_count,
                                                   double threshold) {
    if (!GEOMETRIC_IS_VALID(state)) {
        return GEOMETRIC_ERROR_NOT_INITIALIZED;
    }
    if ((!left_modes && left_count > 0) || (!right_modes && right_count > 0)) {
        return GEOMETRIC_ERROR_INVALID_INPUT;
    }

    double noise_floor = 1e-6;
    double left_energy = 0.0;
    double right_energy = 0.0;
    int32_t left_active = 0;
    int32_t right_active = 0;
    int32_t left_zero_modes = 0;
    int32_t right_zero_modes = 0;

    /* Analyze left modes */
    for (uint32_t i = 0; i < left_count && i < GEOMETRIC_MAX_MODES; i++) {
        double amp = left_modes[i];
        double energy = amp * amp;
        left_energy += energy;
        if (fast_abs(amp) > noise_floor) {
            left_active++;
        }
        /* Zero modes: near noise floor */
        if (fast_abs(amp) <= noise_floor * 10.0 && fast_abs(amp) > noise_floor * 0.1) {
            left_zero_modes++;
        }
    }

    /* Analyze right modes */
    for (uint32_t i = 0; i < right_count && i < GEOMETRIC_MAX_MODES; i++) {
        double amp = right_modes[i];
        double energy = amp * amp;
        right_energy += energy;
        if (fast_abs(amp) > noise_floor) {
            right_active++;
        }
        if (fast_abs(amp) <= noise_floor * 10.0 && fast_abs(amp) > noise_floor * 0.1) {
            right_zero_modes++;
        }
    }

    /* Anomaly index */
    int32_t anomaly_index = right_active - left_active;

    /* Spectral asymmetry: η = Σ|right| - Σ|left| */
    double spectral_asymmetry = 0.0;
    for (uint32_t i = 0; i < right_count && i < GEOMETRIC_MAX_MODES; i++) {
        spectral_asymmetry += fast_abs(right_modes[i]);
    }
    for (uint32_t i = 0; i < left_count && i < GEOMETRIC_MAX_MODES; i++) {
        spectral_asymmetry -= fast_abs(left_modes[i]);
    }

    /* Topological charge: Q = (1/2)(N_right - N_left) */
    double topological_charge = 0.5 * (double)(right_zero_modes - left_zero_modes);

    /* Asymmetry ratio */
    double total_energy = left_energy + right_energy;
    double asymmetry_ratio = 0.0;
    if (total_energy > 1e-15) {
        asymmetry_ratio = fast_abs(right_energy - left_energy) / total_energy;
    }

    bool is_anomalous = asymmetry_ratio > threshold;

    /* Store result */
    state->last_anomaly.anomaly_index = anomaly_index;
    state->last_anomaly.spectral_asymmetry = spectral_asymmetry;
    state->last_anomaly.topological_charge = topological_charge;
    state->last_anomaly.is_anomalous = is_anomalous;
    state->last_anomaly.left_energy = left_energy;
    state->last_anomaly.right_energy = right_energy;
    state->last_anomaly.asymmetry_ratio = asymmetry_ratio;

    /* Update anomaly history ring buffer */
    state->anomaly_history[state->anomaly_history_idx] = is_anomalous;
    state->anomaly_history_idx = (state->anomaly_history_idx + 1) % GEOMETRIC_BERRY_HISTORY;
    if (state->anomaly_history_count < GEOMETRIC_BERRY_HISTORY) {
        state->anomaly_history_count++;
    }

    return GEOMETRIC_SUCCESS;
}

bool geometric_is_anomalous(const geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state))
        return false;
    if (state->anomaly_history_count < 3)
        return false;

    /* Check last 3 consecutive readings */
    uint32_t consecutive = 0;
    uint32_t idx = state->anomaly_history_idx;
    for (uint32_t i = 0; i < state->anomaly_history_count && i < 3; i++) {
        idx = (idx == 0) ? GEOMETRIC_BERRY_HISTORY - 1 : idx - 1;
        if (state->anomaly_history[idx]) {
            consecutive++;
        } else {
            break;
        }
    }

    return consecutive >= 3;
}

const chiral_anomaly_t *geometric_get_last_anomaly(const geometric_state_t *state) {
    if (!GEOMETRIC_IS_VALID(state))
        return (const chiral_anomaly_t *)0;
    return &state->last_anomaly;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

void geometric_dump_state(const geometric_state_t *state) {
    if (!state) {
        boot_log("Geometric state: NULL");
        return;
    }
    if (!state->initialized) {
        boot_log("Geometric state: not initialized");
        return;
    }

    boot_log("=== Geometric Control State ===");

    boot_log("Dimension: ");
    early_console_write_hex(state->metric.dimension);

    boot_log("Metric updates: ");
    early_console_write_hex(state->metric.update_count);

    /* Ricci scalar (scaled by 1000 for display) */
    boot_log("Ricci scalar (x1000): ");
    int32_t ricci_display = (int32_t)(state->ricci_scalar * 1000.0);
    early_console_write_hex((uint32_t)ricci_display);

    /* Berry phase (scaled by 1000) */
    boot_log("Berry phase (x1000): ");
    int32_t berry_display = (int32_t)(state->berry.phase * 1000.0);
    early_console_write_hex((uint32_t)berry_display);

    /* Berry curvature (scaled by 1000) */
    boot_log("Berry curvature (x1000): ");
    int32_t curv_display = (int32_t)(state->berry.curvature * 1000.0);
    early_console_write_hex((uint32_t)curv_display);

    /* Anomaly state */
    boot_log("Anomalous: ");
    early_console_write_hex(geometric_is_anomalous(state) ? 1 : 0);

    if (state->last_anomaly.is_anomalous) {
        boot_log("Last anomaly index: ");
        early_console_write_hex((uint32_t)state->last_anomaly.anomaly_index);
        boot_log("Asymmetry ratio (x1000): ");
        early_console_write_hex((uint32_t)(state->last_anomaly.asymmetry_ratio * 1000.0));
    }

    /* Metric diagonal (first 4 elements) */
    boot_log("Metric diagonal [0..3]: ");
    uint32_t diag_count = state->metric.dimension < 4 ? state->metric.dimension : 4;
    for (uint32_t i = 0; i < diag_count; i++) {
        early_console_write_hex((uint32_t)(state->metric.g[i][i] * 1000.0));
    }

    boot_log("=== End Geometric State ===");
}
