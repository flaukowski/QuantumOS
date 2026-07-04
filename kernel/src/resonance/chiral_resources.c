/**
 * QuantumOS Chiral-Enhanced Quantum Resource Management Implementation
 *
 * Implements chiral resource allocation and stability management
 * for quantum qubits, integrating ghostOS chiral dynamics with
 * QuantumOS quantum resource infrastructure.
 *
 * Core Equation (Chiral Sine-Gordon):
 *   phi_tt - phi_xx + sin(phi) = -eta*phi_x - gamma*phi_t
 *
 * Stability Classification:
 *   |eta/gamma| < 0.3  => EXCELLENT
 *   |eta/gamma| < 0.6  => GOOD
 *   |eta/gamma| < 1.0  => MARGINAL
 *   |eta/gamma| >= 1.0 => UNSTABLE
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/resonance/chiral_resources.h>
#include <kernel/resonance/resonance_types.h>
#include <kernel/memory.h>
#include <kernel/boot.h>
#include <kernel/types.h>

/* ============================================================================
 * Mathematical Helpers
 * ============================================================================ */

static double fabs_d(double x) {
    return x < 0.0 ? -x : x;
}

static double clamp(double value, double min, double max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/* ============================================================================
 * Internal State
 * ============================================================================ */

#define MAX_CHIRAL_QUBITS 256

static chiral_qubit_t qubit_pool[MAX_CHIRAL_QUBITS];
static chiral_pool_t pool_state;
static bool chiral_resources_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * Classify stability based on |eta/gamma| ratio
 */
static chiral_stability_class_t classify_stability(double asymmetry) {
    if (asymmetry < STABILITY_EXCELLENT)
        return CHIRAL_STABILITY_EXCELLENT;
    if (asymmetry < STABILITY_GOOD)
        return CHIRAL_STABILITY_GOOD;
    if (asymmetry < STABILITY_MARGINAL)
        return CHIRAL_STABILITY_MARGINAL;
    return CHIRAL_STABILITY_UNSTABLE;
}

/**
 * Calculate asymmetry ratio |eta/gamma| for a qubit
 */
static double calculate_asymmetry(double eta, double gamma) {
    if (gamma <= 0.0)
        return fabs_d(eta);
    return fabs_d(eta / gamma);
}

/**
 * Recalculate a qubit's derived stability fields from eta/gamma
 */
static void recalculate_qubit_stability(chiral_qubit_t *q) {
    q->asymmetry = calculate_asymmetry(q->chiral_eta, q->chiral_gamma);
    q->stability_class = classify_stability(q->asymmetry);
}

/**
 * Check if a qubit ID is valid and within range
 */
static bool is_valid_qubit_id(uint32_t qubit_id) {
    return qubit_id < MAX_CHIRAL_QUBITS;
}

/**
 * Recalculate all pool statistics by scanning the qubit pool
 */
static void update_pool_stats(void) {
    uint32_t available = 0;
    uint32_t allocated = 0;
    uint32_t excellent = 0;
    uint32_t good = 0;
    uint32_t marginal = 0;
    uint32_t unstable = 0;
    uint32_t left_count = 0;
    uint32_t right_count = 0;
    uint32_t neutral_count = 0;
    uint32_t ciss_count = 0;
    double total_ciss_boost = 0.0;
    uint32_t topo_count = 0;
    double total_topo_charge = 0.0;
    double total_asymmetry = 0.0;
    uint32_t active_count = 0;

    for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS; i++) {
        chiral_qubit_t *q = &qubit_pool[i];

        if (!q->base.is_allocated) {
            available++;
            continue;
        }

        allocated++;
        active_count++;

        /* Stability class counts */
        switch (q->stability_class) {
        case CHIRAL_STABILITY_EXCELLENT:
            excellent++;
            break;
        case CHIRAL_STABILITY_GOOD:
            good++;
            break;
        case CHIRAL_STABILITY_MARGINAL:
            marginal++;
            break;
        case CHIRAL_STABILITY_UNSTABLE:
            unstable++;
            break;
        }

        /* Handedness counts */
        switch (q->handedness) {
        case HANDEDNESS_LEFT:
            left_count++;
            break;
        case HANDEDNESS_RIGHT:
            right_count++;
            break;
        case HANDEDNESS_NEUTRAL:
            neutral_count++;
            break;
        }

        /* CISS metrics */
        if (q->ciss_active) {
            ciss_count++;
            total_ciss_boost += q->ciss_coherence_boost;
        }

        /* Topological metrics */
        if (q->topologically_protected) {
            topo_count++;
            total_topo_charge += q->topological_charge;
        }

        total_asymmetry += q->asymmetry;
    }

    pool_state.total_qubits = MAX_CHIRAL_QUBITS;
    pool_state.available_qubits = available;
    pool_state.allocated_qubits = allocated;

    pool_state.excellent_qubits = excellent;
    pool_state.good_qubits = good;
    pool_state.marginal_qubits = marginal;
    pool_state.unstable_qubits = unstable;

    pool_state.left_handed_qubits = left_count;
    pool_state.right_handed_qubits = right_count;
    pool_state.neutral_qubits = neutral_count;

    pool_state.ciss_enabled_qubits = ciss_count;
    pool_state.average_ciss_boost =
        (ciss_count > 0) ? (total_ciss_boost / (double)ciss_count) : 1.0;

    pool_state.protected_qubits = topo_count;
    pool_state.total_topological_charge = total_topo_charge;

    pool_state.average_asymmetry =
        (active_count > 0) ? (total_asymmetry / (double)active_count) : 0.0;
    pool_state.pool_stability = classify_stability(pool_state.average_asymmetry);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

status_t chiral_resources_init(void) {
    if (chiral_resources_initialized) {
        return STATUS_SUCCESS;
    }

    boot_log("Initializing chiral resource manager...");

    /* Clear the entire qubit pool */
    memset(qubit_pool, 0, sizeof(qubit_pool));

    /* Clear pool state */
    memset(&pool_state, 0, sizeof(pool_state));

    /* Initialize each qubit with default chiral parameters */
    for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS; i++) {
        chiral_qubit_t *q = &qubit_pool[i];

        q->base.qubit_id = i;
        q->base.is_allocated = 0;
        q->base.coherence_time = 1000000000; /* 1 second default */
        q->base.fidelity = FIDELITY_STANDARD;

        q->chiral_eta = ETA_OPTIMAL;
        q->chiral_gamma = 1.0;
        q->asymmetry = calculate_asymmetry(q->chiral_eta, q->chiral_gamma);
        q->stability_class = classify_stability(q->asymmetry);

        q->topological_charge = 0.0;
        q->energy_gap = 0.0;
        q->topologically_protected = false;

        q->ciss_polarization = 0.0;
        q->ciss_coherence_boost = 1.0;
        q->ciss_active = false;

        q->enhanced_coherence_time = q->base.coherence_time;
        q->enhanced_fidelity = q->base.fidelity;

        q->handedness = HANDEDNESS_NEUTRAL;
        q->coupling_count = 0;
    }

    /* Set pool totals */
    pool_state.total_qubits = MAX_CHIRAL_QUBITS;
    pool_state.available_qubits = MAX_CHIRAL_QUBITS;
    pool_state.allocated_qubits = 0;

    update_pool_stats();

    chiral_resources_initialized = true;

    boot_log("Chiral resource manager initialized with ghostOS dynamics");
    return STATUS_SUCCESS;
}

void chiral_resources_shutdown(void) {
    if (!chiral_resources_initialized)
        return;

    /* Clear all qubit state */
    memset(qubit_pool, 0, sizeof(qubit_pool));
    memset(&pool_state, 0, sizeof(pool_state));

    chiral_resources_initialized = false;
    boot_log("Chiral resource manager shutdown");
}

status_t chiral_get_pool_state(chiral_pool_t *pool) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!pool) {
        return STATUS_INVALID_ARG;
    }

    *pool = pool_state;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Qubit Allocation
 * ============================================================================ */

status_t chiral_allocate(const chiral_alloc_request_t *request, chiral_alloc_result_t *result) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!request || !result) {
        return STATUS_INVALID_ARG;
    }

    /* Clear result */
    memset(result, 0, sizeof(chiral_alloc_result_t));
    result->success = false;

    uint32_t count_needed = request->qubits_requested;
    if (count_needed > MAX_ALLOC_IDS) {
        count_needed = MAX_ALLOC_IDS;
    }

    if (count_needed == 0) {
        result->success = true;
        return STATUS_SUCCESS;
    }

    uint32_t allocated = 0;
    double total_asymmetry = 0.0;
    double total_ciss_boost = 0.0;
    chiral_stability_class_t worst_stability = CHIRAL_STABILITY_EXCELLENT;

    for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS && allocated < count_needed; i++) {
        chiral_qubit_t *q = &qubit_pool[i];

        /* Skip already allocated qubits */
        if (q->base.is_allocated)
            continue;

        /* Check stability requirement */
        if (q->stability_class > request->min_stability)
            continue;

        /* Check asymmetry requirement */
        if (request->max_asymmetry > 0.0 && q->asymmetry > request->max_asymmetry)
            continue;

        /* Check handedness preference */
        if (request->preferred_handedness != HANDEDNESS_NEUTRAL &&
            q->handedness != HANDEDNESS_NEUTRAL && q->handedness != request->preferred_handedness)
            continue;

        /* Check CISS requirement */
        if (request->require_ciss && !q->ciss_active) {
            /* Enable CISS on this qubit if required */
            chiral_enable_ciss(i);
        }

        /* Check topological requirement */
        if (request->require_topological && !q->topologically_protected)
            continue;

        /* Check coherence time requirement */
        if (request->min_coherence_time > 0 &&
            q->enhanced_coherence_time < request->min_coherence_time)
            continue;

        /* Check fidelity requirement */
        if (request->min_fidelity > 0 && q->base.fidelity < request->min_fidelity)
            continue;

        /* This qubit qualifies -- allocate it */
        q->base.is_allocated = 1;

        /* Set handedness if requested */
        if (request->preferred_handedness != HANDEDNESS_NEUTRAL) {
            q->handedness = request->preferred_handedness;
        }

        result->qubit_ids[allocated] = i;
        allocated++;

        total_asymmetry += q->asymmetry;
        total_ciss_boost += q->ciss_coherence_boost;

        if (q->stability_class > worst_stability) {
            worst_stability = q->stability_class;
        }
    }

    result->qubits_allocated = allocated;
    result->achieved_stability = worst_stability;
    result->achieved_asymmetry = (allocated > 0) ? (total_asymmetry / (double)allocated) : 0.0;
    result->ciss_boost = (allocated > 0) ? (total_ciss_boost / (double)allocated) : 1.0;

    if (allocated > 0) {
        result->achieved_coherence = qubit_pool[result->qubit_ids[0]].enhanced_coherence_time;
        result->achieved_fidelity = qubit_pool[result->qubit_ids[0]].base.fidelity;
    }

    /* Set warnings */
    if (allocated < count_needed) {
        result->stability_compromised = true;
    }
    if (request->min_coherence_time > 0 && allocated > 0 &&
        result->achieved_coherence < request->min_coherence_time) {
        result->coherence_reduced = true;
    }

    result->success = (allocated > 0);

    /* Update pool statistics */
    update_pool_stats();

    return (allocated > 0) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

status_t chiral_deallocate(uint32_t pid, const uint32_t *qubit_ids, uint32_t count) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!qubit_ids || count == 0) {
        return STATUS_INVALID_ARG;
    }

    (void)pid; /* Reserved for ownership validation */

    for (uint32_t i = 0; i < count; i++) {
        uint32_t qid = qubit_ids[i];
        if (!is_valid_qubit_id(qid))
            continue;

        chiral_qubit_t *q = &qubit_pool[qid];
        if (!q->base.is_allocated)
            continue;

        /* Reset the qubit to default state */
        q->base.is_allocated = 0;

        q->chiral_eta = ETA_OPTIMAL;
        q->chiral_gamma = 1.0;
        recalculate_qubit_stability(q);

        q->topological_charge = 0.0;
        q->energy_gap = 0.0;
        q->topologically_protected = false;

        q->ciss_polarization = 0.0;
        q->ciss_coherence_boost = 1.0;
        q->ciss_active = false;

        q->enhanced_coherence_time = q->base.coherence_time;
        q->enhanced_fidelity = q->base.fidelity;

        q->handedness = HANDEDNESS_NEUTRAL;

        /* Remove all couplings */
        for (uint8_t c = 0; c < q->coupling_count; c++) {
            uint32_t other_id = q->coupled_qubits[c];
            if (!is_valid_qubit_id(other_id))
                continue;

            chiral_qubit_t *other = &qubit_pool[other_id];
            /* Remove qid from other's coupled list */
            for (uint8_t j = 0; j < other->coupling_count; j++) {
                if (other->coupled_qubits[j] == qid) {
                    for (uint8_t k = j; k < other->coupling_count - 1; k++) {
                        other->coupled_qubits[k] = other->coupled_qubits[k + 1];
                    }
                    other->coupling_count--;
                    break;
                }
            }
        }
        q->coupling_count = 0;
    }

    update_pool_stats();
    return STATUS_SUCCESS;
}

status_t chiral_get_qubit(uint32_t qubit_id, chiral_qubit_t *qubit) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id) || !qubit) {
        return STATUS_INVALID_ARG;
    }

    *qubit = qubit_pool[qubit_id];
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Stability Management
 * ============================================================================ */

status_t chiral_optimize_qubit(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    /* Move eta toward optimal value (phi inverse ~0.618) by 10% per call */
    double target_eta = ETA_OPTIMAL;
    q->chiral_eta = 0.9 * q->chiral_eta + 0.1 * target_eta;

    /* Adjust gamma to maintain stability */
    if (q->chiral_gamma > 0.0) {
        double ratio = fabs_d(q->chiral_eta / q->chiral_gamma);
        if (ratio >= STABILITY_MARGINAL) {
            q->chiral_gamma = fabs_d(q->chiral_eta) / (STABILITY_MARGINAL * 0.9);
        }
    }

    recalculate_qubit_stability(q);

    return STATUS_SUCCESS;
}

status_t chiral_rebalance_qubit(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    if (q->asymmetry > STABILITY_MARGINAL) {
        /* Unstable: increase gamma to reduce |eta/gamma| ratio */
        q->chiral_gamma = fabs_d(q->chiral_eta) / (STABILITY_GOOD * 0.9);
    } else if (q->asymmetry < STABILITY_EXCELLENT) {
        /* Over-damped: relax gamma slightly to allow more dynamics */
        q->chiral_gamma *= 0.95;
        if (q->chiral_gamma < 0.1) {
            q->chiral_gamma = 0.1;
        }
    }

    recalculate_qubit_stability(q);

    return STATUS_SUCCESS;
}

status_t chiral_flip_handedness(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    switch (q->handedness) {
    case HANDEDNESS_LEFT:
        q->handedness = HANDEDNESS_RIGHT;
        break;
    case HANDEDNESS_RIGHT:
        q->handedness = HANDEDNESS_LEFT;
        break;
    case HANDEDNESS_NEUTRAL:
        /* Neutral stays neutral */
        break;
    }

    return STATUS_SUCCESS;
}

chiral_stability_class_t chiral_get_stability(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return CHIRAL_STABILITY_UNSTABLE;
    }

    return qubit_pool[qubit_id].stability_class;
}

bool chiral_is_stable(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return false;
    }

    return qubit_pool[qubit_id].stability_class != CHIRAL_STABILITY_UNSTABLE;
}

/* ============================================================================
 * CISS Enhancement
 * ============================================================================ */

status_t chiral_enable_ciss(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    q->ciss_active = true;
    q->ciss_polarization = CISS_SPIN_SELECTIVITY;
    q->ciss_coherence_boost = CISS_COHERENCE_FACTOR;

    /* Enhanced coherence = base * CISS factor */
    q->enhanced_coherence_time = (uint64_t)((double)q->base.coherence_time * CISS_COHERENCE_FACTOR);

    /* Enhanced fidelity = base * CISS fidelity factor (capped at 10000) */
    uint32_t new_fidelity = (uint32_t)((double)q->base.fidelity * CISS_FIDELITY_FACTOR);
    q->enhanced_fidelity = (new_fidelity > 10000) ? 10000 : new_fidelity;

    return STATUS_SUCCESS;
}

status_t chiral_disable_ciss(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    q->ciss_active = false;
    q->ciss_polarization = 0.0;
    q->ciss_coherence_boost = 1.0;

    /* Revert to base values */
    q->enhanced_coherence_time = q->base.coherence_time;
    q->enhanced_fidelity = q->base.fidelity;

    return STATUS_SUCCESS;
}

double chiral_get_ciss_boost(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return 1.0;
    }

    return qubit_pool[qubit_id].ciss_coherence_boost;
}

uint64_t chiral_get_enhanced_coherence(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return 0;
    }

    return qubit_pool[qubit_id].enhanced_coherence_time;
}

/* ============================================================================
 * Topological Protection
 * ============================================================================ */

status_t chiral_enable_topological(uint32_t qubit_id, double target_charge) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    /* Clamp charge to valid range */
    q->topological_charge = clamp(target_charge, TOPOLOGICAL_CHARGE_MIN, TOPOLOGICAL_CHARGE_MAX);

    q->energy_gap = TOPOLOGICAL_ENERGY_GAP;
    q->topologically_protected = true;

    return STATUS_SUCCESS;
}

status_t chiral_disable_topological(uint32_t qubit_id) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit_id)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    q->topological_charge = 0.0;
    q->energy_gap = 0.0;
    q->topologically_protected = false;

    return STATUS_SUCCESS;
}

double chiral_get_topological_charge(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return 0.0;
    }

    return qubit_pool[qubit_id].topological_charge;
}

bool chiral_is_topologically_protected(uint32_t qubit_id) {
    if (!chiral_resources_initialized || !is_valid_qubit_id(qubit_id)) {
        return false;
    }

    chiral_qubit_t *q = &qubit_pool[qubit_id];

    return q->topologically_protected && q->topological_charge >= TOPOLOGICAL_CHARGE_MIN &&
           q->energy_gap >= TOPOLOGICAL_ENERGY_GAP;
}

/* ============================================================================
 * Coupling Operations
 * ============================================================================ */

status_t chiral_couple_qubits(uint32_t qubit1, uint32_t qubit2) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit1) || !is_valid_qubit_id(qubit2)) {
        return STATUS_INVALID_ARG;
    }

    if (qubit1 == qubit2) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q1 = &qubit_pool[qubit1];
    chiral_qubit_t *q2 = &qubit_pool[qubit2];

    /* Check coupling capacity (max 4 per qubit) */
    if (q1->coupling_count >= 4 || q2->coupling_count >= 4) {
        return STATUS_ERROR;
    }

    /* Check if already coupled */
    for (uint8_t i = 0; i < q1->coupling_count; i++) {
        if (q1->coupled_qubits[i] == qubit2) {
            return STATUS_SUCCESS; /* Already coupled */
        }
    }

    /* Add coupling in both directions */
    q1->coupled_qubits[q1->coupling_count++] = qubit2;
    q2->coupled_qubits[q2->coupling_count++] = qubit1;

    return STATUS_SUCCESS;
}

status_t chiral_decouple_qubits(uint32_t qubit1, uint32_t qubit2) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (!is_valid_qubit_id(qubit1) || !is_valid_qubit_id(qubit2)) {
        return STATUS_INVALID_ARG;
    }

    chiral_qubit_t *q1 = &qubit_pool[qubit1];
    chiral_qubit_t *q2 = &qubit_pool[qubit2];

    /* Remove qubit2 from q1's coupled list */
    for (uint8_t i = 0; i < q1->coupling_count; i++) {
        if (q1->coupled_qubits[i] == qubit2) {
            for (uint8_t j = i; j < q1->coupling_count - 1; j++) {
                q1->coupled_qubits[j] = q1->coupled_qubits[j + 1];
            }
            q1->coupling_count--;
            break;
        }
    }

    /* Remove qubit1 from q2's coupled list */
    for (uint8_t i = 0; i < q2->coupling_count; i++) {
        if (q2->coupled_qubits[i] == qubit1) {
            for (uint8_t j = i; j < q2->coupling_count - 1; j++) {
                q2->coupled_qubits[j] = q2->coupled_qubits[j + 1];
            }
            q2->coupling_count--;
            break;
        }
    }

    return STATUS_SUCCESS;
}

double chiral_get_coupling_strength(uint32_t qubit1, uint32_t qubit2) {
    if (!chiral_resources_initialized) {
        return 0.0;
    }

    if (!is_valid_qubit_id(qubit1) || !is_valid_qubit_id(qubit2)) {
        return 0.0;
    }

    chiral_qubit_t *q1 = &qubit_pool[qubit1];

    /* Check if coupled */
    bool coupled = false;
    for (uint8_t i = 0; i < q1->coupling_count; i++) {
        if (q1->coupled_qubits[i] == qubit2) {
            coupled = true;
            break;
        }
    }

    if (!coupled) {
        return 0.0;
    }

    /*
     * Coupling strength derived from chiral parameters:
     * strength = 1.0 - average_asymmetry (higher stability = stronger coupling)
     * Boosted by CISS if both qubits have it active
     */
    chiral_qubit_t *q2 = &qubit_pool[qubit2];
    double avg_asymmetry = (q1->asymmetry + q2->asymmetry) / 2.0;
    double strength = 1.0 - clamp(avg_asymmetry, 0.0, 1.0);

    /* CISS boost to coupling */
    if (q1->ciss_active && q2->ciss_active) {
        strength *= CISS_FIDELITY_FACTOR;
        if (strength > 1.0)
            strength = 1.0;
    }

    return strength;
}

/* ============================================================================
 * Pool Optimization
 * ============================================================================ */

status_t chiral_optimize_pool(void) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS; i++) {
        if (qubit_pool[i].base.is_allocated) {
            chiral_optimize_qubit(i);
        }
    }

    update_pool_stats();
    return STATUS_SUCCESS;
}

uint32_t chiral_rebalance_pool(void) {
    if (!chiral_resources_initialized) {
        return 0;
    }

    uint32_t rebalanced = 0;

    for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS; i++) {
        if (qubit_pool[i].base.is_allocated) {
            chiral_stability_class_t before = qubit_pool[i].stability_class;
            chiral_rebalance_qubit(i);
            if (qubit_pool[i].stability_class != before) {
                rebalanced++;
            }
        }
    }

    update_pool_stats();
    return rebalanced;
}

status_t chiral_get_pool_stability(double *average_asymmetry,
                                   chiral_stability_class_t *stability_class) {
    if (!chiral_resources_initialized) {
        return STATUS_ERROR;
    }

    if (average_asymmetry) {
        *average_asymmetry = pool_state.average_asymmetry;
    }

    if (stability_class) {
        *stability_class = pool_state.pool_stability;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * String Helpers (for diagnostics)
 * ============================================================================ */

/**
 * Simple integer-to-hex string helper for stats output.
 * Writes up to 8 hex digits followed by a null terminator.
 * Returns the number of characters written (excluding null).
 */
static size_t uint_to_hex(uint32_t value, char *buf, size_t buf_size) {
    static const char hex_chars[] = "0123456789ABCDEF";
    char tmp[9];
    size_t len = 0;

    if (buf_size == 0)
        return 0;

    if (value == 0) {
        if (buf_size >= 2) {
            buf[0] = '0';
            buf[1] = '\0';
            return 1;
        }
        buf[0] = '\0';
        return 0;
    }

    /* Build hex string in reverse */
    while (value > 0 && len < 8) {
        tmp[len++] = hex_chars[value & 0xF];
        value >>= 4;
    }

    /* Reverse into output buffer */
    size_t written = 0;
    while (len > 0 && written < buf_size - 1) {
        buf[written++] = tmp[--len];
    }
    buf[written] = '\0';

    return written;
}

/**
 * Simple string copy helper. Returns number of chars written.
 */
static size_t str_copy(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    if (dst_size == 0) {
        return 0;
    }
    /* bounds check before the index is used; also keeps dst_size - 1 from
     * underflowing when dst_size == 0 */
    while (i < dst_size - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

void chiral_dump_qubit(int32_t qubit_id) {
    if (!chiral_resources_initialized) {
        boot_log("Chiral resources not initialized");
        return;
    }

    if (qubit_id >= 0) {
        if ((uint32_t)qubit_id >= MAX_CHIRAL_QUBITS) {
            boot_log("Invalid chiral qubit ID");
            return;
        }

        chiral_qubit_t *q = &qubit_pool[(uint32_t)qubit_id];

        boot_log("=== Chiral Qubit State ===");
        boot_log("Qubit ID: ");
        early_console_write_hex(q->base.qubit_id);
        boot_log("Allocated: ");
        early_console_write_hex(q->base.is_allocated);
        boot_log("Eta (x1000): ");
        early_console_write_hex((uint32_t)(q->chiral_eta * 1000));
        boot_log("Gamma (x1000): ");
        early_console_write_hex((uint32_t)(q->chiral_gamma * 1000));
        boot_log("Asymmetry (x1000): ");
        early_console_write_hex((uint32_t)(q->asymmetry * 1000));
        boot_log("Stability class: ");
        early_console_write_hex(q->stability_class);
        boot_log("Handedness: ");
        early_console_write_hex(q->handedness);
        boot_log("CISS active: ");
        early_console_write_hex(q->ciss_active);
        boot_log("CISS boost (x1000): ");
        early_console_write_hex((uint32_t)(q->ciss_coherence_boost * 1000));
        boot_log("Topological charge (x1000): ");
        early_console_write_hex((uint32_t)(q->topological_charge * 1000));
        boot_log("Topologically protected: ");
        early_console_write_hex(q->topologically_protected);
        boot_log("Coherence time: ");
        early_console_write_hex((uint32_t)(q->enhanced_coherence_time >> 32));
        early_console_write_hex((uint32_t)(q->enhanced_coherence_time & 0xFFFFFFFF));
        boot_log("Coupling count: ");
        early_console_write_hex(q->coupling_count);
    } else {
        /* Dump all allocated qubits */
        boot_log("=== All Chiral Qubits ===");
        for (uint32_t i = 0; i < MAX_CHIRAL_QUBITS; i++) {
            if (qubit_pool[i].base.is_allocated) {
                chiral_dump_qubit((int32_t)i);
            }
        }
    }
}

void chiral_dump_pool(void) {
    if (!chiral_resources_initialized) {
        boot_log("Chiral resources not initialized");
        return;
    }

    boot_log("=== Chiral Pool State ===");
    boot_log("Total qubits: ");
    early_console_write_hex(pool_state.total_qubits);
    boot_log("Available qubits: ");
    early_console_write_hex(pool_state.available_qubits);
    boot_log("Allocated qubits: ");
    early_console_write_hex(pool_state.allocated_qubits);

    boot_log("-- By stability class --");
    boot_log("Excellent: ");
    early_console_write_hex(pool_state.excellent_qubits);
    boot_log("Good: ");
    early_console_write_hex(pool_state.good_qubits);
    boot_log("Marginal: ");
    early_console_write_hex(pool_state.marginal_qubits);
    boot_log("Unstable: ");
    early_console_write_hex(pool_state.unstable_qubits);

    boot_log("-- By handedness --");
    boot_log("Left: ");
    early_console_write_hex(pool_state.left_handed_qubits);
    boot_log("Right: ");
    early_console_write_hex(pool_state.right_handed_qubits);
    boot_log("Neutral: ");
    early_console_write_hex(pool_state.neutral_qubits);

    boot_log("-- CISS metrics --");
    boot_log("CISS enabled: ");
    early_console_write_hex(pool_state.ciss_enabled_qubits);
    boot_log("Average CISS boost (x1000): ");
    early_console_write_hex((uint32_t)(pool_state.average_ciss_boost * 1000));

    boot_log("-- Topological metrics --");
    boot_log("Protected qubits: ");
    early_console_write_hex(pool_state.protected_qubits);
    boot_log("Total charge (x1000): ");
    early_console_write_hex((uint32_t)(pool_state.total_topological_charge * 1000));

    boot_log("-- Global stability --");
    boot_log("Average asymmetry (x1000): ");
    early_console_write_hex((uint32_t)(pool_state.average_asymmetry * 1000));
    boot_log("Pool stability class: ");
    early_console_write_hex(pool_state.pool_stability);
}

size_t chiral_get_stats_string(char *buffer, size_t size) {
    if (!chiral_resources_initialized || !buffer || size == 0) {
        if (buffer && size > 0)
            buffer[0] = '\0';
        return 0;
    }

    size_t pos = 0;
    size_t remaining;
    size_t written;

#define STATS_APPEND_STR(s)                                                                        \
    do {                                                                                           \
        remaining = (pos < size) ? (size - pos) : 0;                                               \
        if (remaining > 1) {                                                                       \
            written = str_copy(buffer + pos, remaining, (s));                                      \
            pos += written;                                                                        \
        }                                                                                          \
    } while (0)

#define STATS_APPEND_HEX(v)                                                                        \
    do {                                                                                           \
        remaining = (pos < size) ? (size - pos) : 0;                                               \
        if (remaining > 1) {                                                                       \
            written = uint_to_hex((uint32_t)(v), buffer + pos, remaining);                         \
            pos += written;                                                                        \
        }                                                                                          \
    } while (0)

    STATS_APPEND_STR("Chiral Pool: total=");
    STATS_APPEND_HEX(pool_state.total_qubits);
    STATS_APPEND_STR(" avail=");
    STATS_APPEND_HEX(pool_state.available_qubits);
    STATS_APPEND_STR(" alloc=");
    STATS_APPEND_HEX(pool_state.allocated_qubits);

    STATS_APPEND_STR(" | stability: exc=");
    STATS_APPEND_HEX(pool_state.excellent_qubits);
    STATS_APPEND_STR(" good=");
    STATS_APPEND_HEX(pool_state.good_qubits);
    STATS_APPEND_STR(" marg=");
    STATS_APPEND_HEX(pool_state.marginal_qubits);
    STATS_APPEND_STR(" unst=");
    STATS_APPEND_HEX(pool_state.unstable_qubits);

    STATS_APPEND_STR(" | hand: L=");
    STATS_APPEND_HEX(pool_state.left_handed_qubits);
    STATS_APPEND_STR(" R=");
    STATS_APPEND_HEX(pool_state.right_handed_qubits);
    STATS_APPEND_STR(" N=");
    STATS_APPEND_HEX(pool_state.neutral_qubits);

    STATS_APPEND_STR(" | ciss=");
    STATS_APPEND_HEX(pool_state.ciss_enabled_qubits);
    STATS_APPEND_STR(" topo=");
    STATS_APPEND_HEX(pool_state.protected_qubits);

    STATS_APPEND_STR(" | asym=");
    STATS_APPEND_HEX((uint32_t)(pool_state.average_asymmetry * 1000));
    STATS_APPEND_STR(" class=");
    STATS_APPEND_HEX(pool_state.pool_stability);

#undef STATS_APPEND_STR
#undef STATS_APPEND_HEX

    return pos;
}
