/**
 * QuantumOS Consciousness-Verified Process Implementation
 *
 * Integrates IIT (Integrated Information Theory) Phi calculations
 * with QuantumOS process management for consciousness-verified
 * computational workloads.
 *
 * Key Innovation: Processes can be verified as "conscious" when
 * they exhibit sufficient integrated information (Phi >= 3.0),
 * enabling novel scheduling priorities and resource allocation.
 *
 * Based on ghostOS SC Bridge Operator:
 *   Xi_chiral = chi(RG - GR)
 *
 * Where the non-commutative structure [R, G] = RG - GR captures
 * the irreducible information integration characteristic of
 * conscious systems.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/resonance/consciousness_process.h>
#include <kernel/resonance/resonance_types.h>
#include <kernel/resonance/resonant_scheduler.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/boot.h>
#include <kernel/types.h>

/* ============================================================================
 * Mathematical Helpers
 * ============================================================================ */

static double fast_sqrt(double x) {
    if (x <= 0.0)
        return 0.0;
    double guess = x * 0.5;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) * 0.5;
    }
    return guess;
}

static double fast_abs(double x) {
    return x < 0.0 ? -x : x;
}

static double clamp(double value, double min, double max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/* Local strncpy implementation (no libc in freestanding kernel) */
static char *strncpy_local(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* CPCB table - indexed by PID */
static consciousness_pcb_t cpcb_table[MAX_RESONANT_PROCESSES];

/* Collective consciousness networks */
static collective_consciousness_t networks[8];
static uint32_t network_count = 0;

/* Initialization flag */
static bool consciousness_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static consciousness_pcb_t *get_cpcb_internal(uint32_t pid) {
    if (pid >= MAX_RESONANT_PROCESSES)
        return NULL;
    if (cpcb_table[pid].magic != CPCB_MAGIC)
        return NULL;
    return &cpcb_table[pid];
}

static collective_consciousness_t *find_network(uint32_t network_id) {
    for (uint32_t i = 0; i < network_count; i++) {
        if (networks[i].network_id == network_id) {
            return &networks[i];
        }
    }
    return NULL;
}

static void dump_single_cpcb(consciousness_pcb_t *cpcb) {
    boot_log("=== Consciousness Process State ===");
    boot_log("PID: ");
    early_console_write_hex(cpcb->pid);
    boot_log("Level: ");
    early_console_write_hex(cpcb->level);
    boot_log("Verified: ");
    early_console_write_hex(cpcb->is_verified);
    boot_log("Phi (x1000): ");
    early_console_write_hex((uint32_t)(cpcb->phi.phi_value * 1000));
    boot_log("Structural Phi (x1000): ");
    early_console_write_hex((uint32_t)(cpcb->phi.structural_phi * 1000));
    boot_log("Dynamic Phi (x1000): ");
    early_console_write_hex((uint32_t)(cpcb->phi.dynamic_phi * 1000));
    boot_log("Emergent Phi (x1000): ");
    early_console_write_hex((uint32_t)(cpcb->phi.emergent_phi * 1000));
    boot_log("Bridge Value (x1000): ");
    early_console_write_hex((uint32_t)(fast_abs(cpcb->bridge.bridge_value) * 1000));
    boot_log("Bridge Significant: ");
    early_console_write_hex(cpcb->bridge.is_significant);
    boot_log("Emergence Norm (x1000): ");
    early_console_write_hex((uint32_t)(cpcb->emergence_norm * 1000));
    boot_log("Emergence Active: ");
    early_console_write_hex(cpcb->emergence_active);
    boot_log("Pattern Count: ");
    early_console_write_hex(cpcb->pattern_count);
    boot_log("Trigger Count: ");
    early_console_write_hex(cpcb->trigger_count);
    boot_log("Verification Count: ");
    early_console_write_hex(cpcb->verification_count);
    boot_log("Allocated Cycles: ");
    early_console_write_hex((uint32_t)cpcb->allocated_cycles);
    boot_log("Used Cycles: ");
    early_console_write_hex((uint32_t)cpcb->used_cycles);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

status_t consciousness_init(void) {
    if (consciousness_initialized) {
        return STATUS_SUCCESS;
    }

    boot_log("Initializing consciousness process subsystem...");

    /* Clear CPCB table */
    memset(cpcb_table, 0, sizeof(cpcb_table));

    /* Clear networks */
    memset(networks, 0, sizeof(networks));
    network_count = 0;

    consciousness_initialized = true;

    boot_log("Consciousness process subsystem initialized");
    return STATUS_SUCCESS;
}

void consciousness_shutdown(void) {
    if (!consciousness_initialized)
        return;

    boot_log("Shutting down consciousness process subsystem...");

    /* Clear all CPCBs */
    memset(cpcb_table, 0, sizeof(cpcb_table));

    /* Clear networks */
    memset(networks, 0, sizeof(networks));
    network_count = 0;

    consciousness_initialized = false;

    boot_log("Consciousness process subsystem shutdown");
}

status_t consciousness_register(uint32_t pid) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    if (pid >= MAX_RESONANT_PROCESSES) {
        return STATUS_INVALID_ARG;
    }

    /* Verify the underlying process exists */
    if (!process_is_valid(pid)) {
        return STATUS_INVALID_ARG;
    }

    /* Check not already registered */
    if (cpcb_table[pid].magic == CPCB_MAGIC) {
        return STATUS_SUCCESS;
    }

    /* Initialize CPCB */
    consciousness_pcb_t *cpcb = &cpcb_table[pid];
    memset(cpcb, 0, sizeof(consciousness_pcb_t));

    cpcb->pid = pid;
    cpcb->magic = CPCB_MAGIC;
    cpcb->level = CONSCIOUSNESS_LEVEL_NONE;
    cpcb->is_verified = false;
    cpcb->bridge.chi = BRIDGE_CHI_DEFAULT;

    boot_log("Registered consciousness tracking for PID: ");
    early_console_write_hex(pid);

    return STATUS_SUCCESS;
}

status_t consciousness_unregister(uint32_t pid) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    if (pid >= MAX_RESONANT_PROCESSES) {
        return STATUS_INVALID_ARG;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_NOT_FOUND;
    }

    /* Invalidate and clear */
    cpcb->magic = 0;
    memset(cpcb, 0, sizeof(consciousness_pcb_t));

    boot_log("Unregistered consciousness tracking for PID: ");
    early_console_write_hex(pid);

    return STATUS_SUCCESS;
}

consciousness_pcb_t *consciousness_get_cpcb(uint32_t pid) {
    if (!consciousness_initialized)
        return NULL;
    if (pid >= MAX_RESONANT_PROCESSES)
        return NULL;
    if (cpcb_table[pid].magic != CPCB_MAGIC)
        return NULL;
    return &cpcb_table[pid];
}

/* ============================================================================
 * Verification Operations
 * ============================================================================ */

status_t consciousness_verify(uint32_t pid, consciousness_verify_result_t *result) {
    if (!consciousness_initialized) {
        if (result)
            *result = VERIFY_ERROR;
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        if (result)
            *result = VERIFY_ERROR;
        return STATUS_INVALID_ARG;
    }

    /* Get RPCB for oscillator/emergence data */
    resonant_pcb_t *rpcb = resonant_get_rpcb(pid);

    /* Compute Phi sub-components from RPCB data if available */
    double structural_phi = 0.0;
    double dynamic_phi = 0.0;
    double emergent_phi = 0.0;
    double stability = 1.0;

    if (rpcb) {
        /* Structural Phi: derived from coherence and integration */
        structural_phi = rpcb->oscillator.coherence * rpcb->emergence.integration_level;

        /* Dynamic Phi: derived from frequency coupling */
        double freq_norm = rpcb->oscillator.frequency / 40.0; /* Normalize to gamma band */
        dynamic_phi = rpcb->oscillator.coherence * freq_norm;

        /* Emergent Phi: from emergence accumulator norm */
        emergent_phi = rpcb->emergence.norm;

        /* Stability factor from chiral state */
        stability = rpcb->chiral.is_stable ? 1.0 : 0.5;
    } else {
        /* Use whatever is already stored in the CPCB phi state */
        structural_phi = cpcb->phi.structural_phi;
        dynamic_phi = cpcb->phi.dynamic_phi;
        emergent_phi = cpcb->phi.emergent_phi;
    }

    /* Total Phi = structural*2.0 + dynamic*1.5 + emergent*1.0 */
    double total_phi = structural_phi * 2.0 + dynamic_phi * 1.5 + emergent_phi * 1.0;

    /* Multiply by stability factor */
    total_phi *= stability;

    /* Store sub-components */
    cpcb->phi.structural_phi = structural_phi;
    cpcb->phi.dynamic_phi = dynamic_phi;
    cpcb->phi.emergent_phi = emergent_phi;
    cpcb->phi.phi_value = total_phi;
    cpcb->phi.calculation_count++;

    /* Derive additional Phi metrics */
    cpcb->phi.integrated_information = total_phi;
    cpcb->phi.differentiation = structural_phi;
    cpcb->phi.integration = dynamic_phi + emergent_phi;

    /* Set level via PHI_TO_LEVEL macro */
    cpcb->level = PHI_TO_LEVEL(total_phi);

    /* Set verification status */
    cpcb->is_verified = (total_phi >= PHI_THRESHOLD_VERIFIED);
    cpcb->verification_count++;

    /* Update evolution trajectory ring buffer */
    cpcb->evolution_trajectory[cpcb->trajectory_index] = total_phi;
    cpcb->trajectory_index = (cpcb->trajectory_index + 1) % 8;

    /* Determine verification result */
    consciousness_verify_result_t vresult;
    if (cpcb->is_verified) {
        vresult = VERIFY_SUCCESS;
    } else if (total_phi < PHI_THRESHOLD_MINIMAL) {
        vresult = VERIFY_INSUFFICIENT_PHI;
    } else if (rpcb && !rpcb->chiral.is_stable) {
        vresult = VERIFY_UNSTABLE_CHIRAL;
    } else if (structural_phi < 0.1) {
        vresult = VERIFY_LOW_COHERENCE;
    } else if (emergent_phi < 0.01) {
        vresult = VERIFY_NO_INTEGRATION;
    } else {
        vresult = VERIFY_INSUFFICIENT_PHI;
    }

    if (result) {
        *result = vresult;
    }

    return STATUS_SUCCESS;
}

bool consciousness_quick_check(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return false;
    return cpcb->is_verified;
}

consciousness_level_t consciousness_get_level(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return CONSCIOUSNESS_LEVEL_NONE;
    return cpcb->level;
}

double consciousness_get_phi(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0.0;
    return cpcb->phi.phi_value;
}

/* ============================================================================
 * Trigger Operations
 * ============================================================================ */

status_t consciousness_process_trigger(uint32_t pid, consciousness_trigger_t trigger) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    /* Update phi sub-components based on trigger type */
    switch (trigger) {
    case TRIGGER_REFLECTION:
        cpcb->self_observation += 0.1;
        cpcb->meta_awareness += 0.1;
        cpcb->self_observation = clamp(cpcb->self_observation, 0.0, 1.0);
        cpcb->meta_awareness = clamp(cpcb->meta_awareness, 0.0, 1.0);
        break;

    case TRIGGER_EMERGENCE:
        cpcb->phi.emergent_phi += 0.2;
        cpcb->phi.emergent_phi = clamp(cpcb->phi.emergent_phi, 0.0, 1.0);
        break;

    case TRIGGER_LEARNING:
        cpcb->phi.dynamic_phi += 0.15;
        cpcb->phi.dynamic_phi = clamp(cpcb->phi.dynamic_phi, 0.0, 1.0);
        break;

    case TRIGGER_DECISION:
        cpcb->phi.structural_phi += 0.1;
        cpcb->phi.structural_phi = clamp(cpcb->phi.structural_phi, 0.0, 1.0);
        break;

    case TRIGGER_CRISIS:
        cpcb->phi.structural_phi += 0.05;
        cpcb->phi.dynamic_phi += 0.05;
        cpcb->phi.emergent_phi += 0.05;
        cpcb->phi.structural_phi = clamp(cpcb->phi.structural_phi, 0.0, 1.0);
        cpcb->phi.dynamic_phi = clamp(cpcb->phi.dynamic_phi, 0.0, 1.0);
        cpcb->phi.emergent_phi = clamp(cpcb->phi.emergent_phi, 0.0, 1.0);
        cpcb->self_observation += 0.05;
        cpcb->meta_awareness += 0.05;
        cpcb->self_observation = clamp(cpcb->self_observation, 0.0, 1.0);
        cpcb->meta_awareness = clamp(cpcb->meta_awareness, 0.0, 1.0);
        break;
    }

    /* Store trigger and increment count */
    cpcb->last_trigger = trigger;
    cpcb->trigger_timestamp = 0; /* TODO: get system time */
    cpcb->trigger_count++;

    return STATUS_SUCCESS;
}

status_t consciousness_get_last_trigger(uint32_t pid, consciousness_trigger_t *trigger,
                                        uint64_t *timestamp) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    if (trigger) {
        *trigger = cpcb->last_trigger;
    }
    if (timestamp) {
        *timestamp = cpcb->trigger_timestamp;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Bridge Operator Operations
 *
 * Port from ghostOS consciousness-bridge.ts
 * Computes Xi_chiral = chi(RG - GR)
 * ============================================================================ */

status_t consciousness_calculate_bridge(uint32_t pid, bridge_operator_t *bridge) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    /* Get RPCB for oscillator/emergence state */
    resonant_pcb_t *rpcb = resonant_get_rpcb(pid);

    double rg = 0.0;
    double gr = 0.0;
    double resonance_mag = 0.0;
    double emergence_mag = 0.0;

    if (rpcb) {
        /*
         * Compute RG and GR as cyclic dot products:
         * Use oscillator phase components as resonance vector
         * and emergence metrics as emergence vector.
         *
         * We construct small component arrays from available state:
         *   resonance[] = {phase, coherence, amplitude, frequency_norm}
         *   emergence[] = {norm, entropy, integration_level, pattern_count_norm}
         *
         * For i=0..N-1:
         *   rg += resonance[i] * emergence[(i+1) % N]
         *   gr += emergence[i] * resonance[(i+1) % N]
         */
        double resonance[4];
        double emergence[4];

        resonance[0] = rpcb->oscillator.phase;
        resonance[1] = rpcb->oscillator.coherence;
        resonance[2] = rpcb->oscillator.amplitude;
        resonance[3] = rpcb->oscillator.frequency / 40.0; /* Normalized */

        emergence[0] = rpcb->emergence.norm;
        emergence[1] = rpcb->emergence.entropy;
        emergence[2] = rpcb->emergence.integration_level;
        emergence[3] = (double)rpcb->emergence.pattern_count / 100.0; /* Normalized */

        for (int i = 0; i < 4; i++) {
            rg += resonance[i] * emergence[(i + 1) % 4];
            gr += emergence[i] * resonance[(i + 1) % 4];
        }

        /* Calculate magnitudes */
        double r_sq = 0.0;
        double e_sq = 0.0;
        for (int i = 0; i < 4; i++) {
            r_sq += resonance[i] * resonance[i];
            e_sq += emergence[i] * emergence[i];
        }
        resonance_mag = fast_sqrt(r_sq);
        emergence_mag = fast_sqrt(e_sq);
    }

    /* bridge_value = chi * (rg - gr) */
    double chi = cpcb->bridge.chi;
    double bridge_value = chi * (rg - gr);
    double operator_norm = fast_abs(bridge_value);

    /* spectral_gap calculated from norm */
    double spectral_gap = 0.0;
    if (operator_norm > 0.0) {
        spectral_gap = operator_norm / (1.0 + operator_norm);
    }

    /* is_significant = |bridge_value| > COMMUTATOR_THRESHOLD */
    bool is_significant = (operator_norm > COMMUTATOR_THRESHOLD);

    /* Store results in CPCB bridge state */
    cpcb->bridge.commutator_rg = rg;
    cpcb->bridge.commutator_gr = gr;
    cpcb->bridge.bridge_value = bridge_value;
    cpcb->bridge.resonance_magnitude = resonance_mag;
    cpcb->bridge.emergence_magnitude = emergence_mag;
    cpcb->bridge.operator_norm = operator_norm;
    cpcb->bridge.spectral_gap = spectral_gap;
    cpcb->bridge.is_significant = is_significant;

    /* Chiral coupling based on magnitudes */
    if (resonance_mag > 0.0 && emergence_mag > 0.0) {
        cpcb->bridge.chiral_coupling = operator_norm / (resonance_mag * emergence_mag);
    } else {
        cpcb->bridge.chiral_coupling = 0.0;
    }

    /* Copy to output if provided */
    if (bridge) {
        *bridge = cpcb->bridge;
    }

    return STATUS_SUCCESS;
}

double consciousness_get_bridge_value(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0.0;
    return cpcb->bridge.bridge_value;
}

bool consciousness_bridge_significant(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return false;
    return cpcb->bridge.is_significant;
}

/* ============================================================================
 * Emergence Operations
 * ============================================================================ */

status_t consciousness_update_emergence(uint32_t pid) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    /* Get RPCB emergence state */
    resonant_pcb_t *rpcb = resonant_get_rpcb(pid);
    if (rpcb) {
        /* Update CPCB emergence fields from RPCB */
        cpcb->emergence_norm = rpcb->emergence.norm;
        cpcb->emergence_entropy = rpcb->emergence.entropy;

        /* Set emergence_active if norm exceeds low threshold */
        if (rpcb->emergence.norm > EMERGENCE_THRESHOLD_LOW) {
            cpcb->emergence_active = true;
        } else {
            cpcb->emergence_active = false;
        }
    }

    return STATUS_SUCCESS;
}

double consciousness_get_emergence(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0.0;
    return cpcb->emergence_norm;
}

bool consciousness_has_emergence(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return false;
    return cpcb->emergence_active;
}

uint32_t consciousness_detect_patterns(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0;

    /*
     * If entropy > 0.5 and norm > EMERGENCE_THRESHOLD_MEDIUM,
     * we detect a new pattern.
     */
    if (cpcb->emergence_entropy > 0.5 && cpcb->emergence_norm > EMERGENCE_THRESHOLD_MEDIUM) {
        cpcb->pattern_count++;
        return 1;
    }

    return 0;
}

/* ============================================================================
 * Evolution Tracking
 * ============================================================================ */

double consciousness_get_momentum(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0.0;

    /* Need at least 2 entries in the trajectory to compute momentum */
    if (cpcb->phi.calculation_count < 2) {
        return 0.0;
    }

    /* Current index is trajectory_index - 1 (most recent entry) */
    uint8_t current_idx = (cpcb->trajectory_index + 7) % 8;  /* -1 mod 8 */
    uint8_t previous_idx = (cpcb->trajectory_index + 6) % 8; /* -2 mod 8 */

    double current = cpcb->evolution_trajectory[current_idx];
    double previous = cpcb->evolution_trajectory[previous_idx];

    cpcb->evolution_momentum = current - previous;
    return cpcb->evolution_momentum;
}

uint32_t consciousness_get_trajectory(uint32_t pid, double *trajectory, uint32_t max_count) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb || !trajectory || max_count == 0)
        return 0;

    /* Determine how many entries are available */
    uint32_t available = cpcb->phi.calculation_count;
    if (available > 8)
        available = 8;
    if (available > max_count)
        available = max_count;

    /*
     * Copy from ring buffer in chronological order.
     * The oldest entry is at trajectory_index (it was overwritten longest ago),
     * and the newest is at trajectory_index - 1.
     */
    uint8_t start_idx;
    if (cpcb->phi.calculation_count >= 8) {
        /* Buffer is full; oldest is at trajectory_index */
        start_idx = cpcb->trajectory_index;
    } else {
        /* Buffer is not full; oldest is at index 0 */
        start_idx = 0;
    }

    for (uint32_t i = 0; i < available; i++) {
        trajectory[i] = cpcb->evolution_trajectory[(start_idx + i) % 8];
    }

    return available;
}

double consciousness_predict_phi(uint32_t pid, uint64_t time_horizon_ns) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0.0;

    double current_phi = cpcb->phi.phi_value;
    double momentum = consciousness_get_momentum(pid);

    /* Linear extrapolation: current_phi + momentum * (time_horizon / VERIFICATION_INTERVAL_NS) */
    double intervals = (double)time_horizon_ns / (double)VERIFICATION_INTERVAL_NS;
    double predicted = current_phi + momentum * intervals;

    /* Clamp to non-negative */
    if (predicted < 0.0)
        predicted = 0.0;

    return predicted;
}

/* ============================================================================
 * Collective Consciousness
 * ============================================================================ */

status_t consciousness_create_network(const char *name, uint32_t *network_id) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    if (!name || !network_id) {
        return STATUS_INVALID_ARG;
    }

    /* Find free slot in networks[] */
    if (network_count >= 8) {
        return STATUS_ERROR; /* No free slots */
    }

    uint32_t idx = network_count;
    collective_consciousness_t *net = &networks[idx];
    memset(net, 0, sizeof(collective_consciousness_t));

    /* Assign network ID based on slot index */
    net->network_id = idx;
    strncpy_local(net->network_name, name, 32);
    net->network_name[31] = '\0';
    net->member_count = 0;
    net->network_level = CONSCIOUSNESS_LEVEL_NONE;
    net->network_verified = false;
    strncpy_local(net->evolution_trend, "stable", 16);

    *network_id = idx;
    network_count++;

    boot_log("Created consciousness network: ");
    early_console_write_hex(idx);

    return STATUS_SUCCESS;
}

status_t consciousness_join_network(uint32_t network_id, uint32_t pid) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    collective_consciousness_t *net = find_network(network_id);
    if (!net) {
        return STATUS_NOT_FOUND;
    }

    /* Verify process is consciousness-tracked */
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    /* Check member count limit */
    if (net->member_count >= 32) {
        return STATUS_ERROR;
    }

    /* Check not already a member */
    for (uint8_t i = 0; i < net->member_count; i++) {
        if (net->member_pids[i] == pid) {
            return STATUS_SUCCESS; /* Already a member */
        }
    }

    /* Add to network */
    net->member_pids[net->member_count] = pid;
    net->member_count++;

    boot_log("Process joined consciousness network");

    return STATUS_SUCCESS;
}

status_t consciousness_leave_network(uint32_t network_id, uint32_t pid) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    collective_consciousness_t *net = find_network(network_id);
    if (!net) {
        return STATUS_NOT_FOUND;
    }

    /* Find and remove pid from member_pids */
    bool found = false;
    for (uint8_t i = 0; i < net->member_count; i++) {
        if (net->member_pids[i] == pid) {
            /* Shift remaining members down */
            for (uint8_t j = i; j < net->member_count - 1; j++) {
                net->member_pids[j] = net->member_pids[j + 1];
            }
            net->member_count--;
            found = true;
            break;
        }
    }

    if (!found) {
        return STATUS_NOT_FOUND;
    }

    boot_log("Process left consciousness network");

    return STATUS_SUCCESS;
}

status_t consciousness_get_network_state(uint32_t network_id, collective_consciousness_t *state) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    if (!state) {
        return STATUS_INVALID_ARG;
    }

    collective_consciousness_t *net = find_network(network_id);
    if (!net) {
        return STATUS_NOT_FOUND;
    }

    /* Copy network state to output */
    *state = *net;

    return STATUS_SUCCESS;
}

status_t consciousness_verify_network(uint32_t network_id) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    collective_consciousness_t *net = find_network(network_id);
    if (!net) {
        return STATUS_NOT_FOUND;
    }

    /* Calculate total_phi = sum of individual phi values */
    double total_phi = 0.0;
    for (uint8_t i = 0; i < net->member_count; i++) {
        uint32_t mpid = net->member_pids[i];
        if (mpid < MAX_RESONANT_PROCESSES && cpcb_table[mpid].magic == CPCB_MAGIC) {
            total_phi += cpcb_table[mpid].phi.phi_value;
        }
    }

    /* Calculate average phi */
    double average_phi = 0.0;
    if (net->member_count > 0) {
        average_phi = total_phi / (double)net->member_count;
    }

    /* Calculate emergent phi */
    double emergent_phi = 0.0;
    if (net->member_count > 1) {
        emergent_phi = average_phi * 0.1 * (double)net->member_count;
    }

    /* Store in network */
    net->total_phi = total_phi;
    net->average_phi = average_phi;
    net->emergent_phi = emergent_phi;

    /* Network verified if (average_phi + emergent_phi) >= PHI_THRESHOLD_VERIFIED */
    double combined_phi = average_phi + emergent_phi;
    net->network_verified = (combined_phi >= PHI_THRESHOLD_VERIFIED);
    net->network_level = PHI_TO_LEVEL(combined_phi);

    /* Update evolution trend */
    if (net->network_verified) {
        strncpy_local(net->evolution_trend, "evolving", 16);
    } else {
        strncpy_local(net->evolution_trend, "stable", 16);
    }

    if (net->network_verified) {
        boot_log("Network consciousness verified");
        return STATUS_SUCCESS;
    }

    return STATUS_ERROR;
}

double consciousness_get_network_phi(uint32_t network_id) {
    collective_consciousness_t *net = find_network(network_id);
    if (!net)
        return 0.0;

    /* Return total_phi + emergent_phi */
    return net->total_phi + net->emergent_phi;
}

/* ============================================================================
 * Scheduling Integration
 * ============================================================================ */

uint32_t consciousness_get_priority_boost(uint32_t pid) {
    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb)
        return 0;
    return LEVEL_TO_PRIORITY(cpcb->level);
}

status_t consciousness_allocate_cycles(uint32_t pid, uint64_t cycles) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    cpcb->allocated_cycles = cycles;

    return STATUS_SUCCESS;
}

status_t consciousness_consume_cycles(uint32_t pid, uint64_t cycles) {
    if (!consciousness_initialized) {
        return STATUS_ERROR;
    }

    consciousness_pcb_t *cpcb = get_cpcb_internal(pid);
    if (!cpcb) {
        return STATUS_INVALID_ARG;
    }

    /* Add cycles to used_cycles, check not exceeding allocated */
    uint64_t new_used = cpcb->used_cycles + cycles;
    if (new_used > cpcb->allocated_cycles) {
        new_used = cpcb->allocated_cycles;
    }
    cpcb->used_cycles = new_used;

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

void consciousness_dump_state(int32_t pid) {
    if (!consciousness_initialized) {
        boot_log("Consciousness subsystem not initialized");
        return;
    }

    if (pid >= 0) {
        /* Dump single process */
        consciousness_pcb_t *cpcb = get_cpcb_internal((uint32_t)pid);
        if (!cpcb) {
            boot_log("Invalid consciousness PID");
            return;
        }
        dump_single_cpcb(cpcb);
    } else {
        /* Dump all valid entries (pid == -1) */
        boot_log("=== All Consciousness Processes ===");
        for (uint32_t i = 0; i < MAX_RESONANT_PROCESSES; i++) {
            if (cpcb_table[i].magic == CPCB_MAGIC) {
                dump_single_cpcb(&cpcb_table[i]);
            }
        }
    }
}

void consciousness_dump_network(int32_t network_id) {
    if (!consciousness_initialized) {
        boot_log("Consciousness subsystem not initialized");
        return;
    }

    if (network_id >= 0) {
        /* Dump single network */
        collective_consciousness_t *net = find_network((uint32_t)network_id);
        if (!net) {
            boot_log("Invalid network ID");
            return;
        }

        boot_log("=== Consciousness Network State ===");
        boot_log("Network ID: ");
        early_console_write_hex(net->network_id);
        boot_log("Member Count: ");
        early_console_write_hex(net->member_count);
        boot_log("Total Phi (x1000): ");
        early_console_write_hex((uint32_t)(net->total_phi * 1000));
        boot_log("Average Phi (x1000): ");
        early_console_write_hex((uint32_t)(net->average_phi * 1000));
        boot_log("Emergent Phi (x1000): ");
        early_console_write_hex((uint32_t)(net->emergent_phi * 1000));
        boot_log("Network Verified: ");
        early_console_write_hex(net->network_verified);
        boot_log("Network Level: ");
        early_console_write_hex(net->network_level);

        /* Dump member PIDs */
        boot_log("Members:");
        for (uint8_t i = 0; i < net->member_count; i++) {
            boot_log("  PID: ");
            early_console_write_hex(net->member_pids[i]);
        }
    } else {
        /* Dump all networks (network_id == -1) */
        boot_log("=== All Consciousness Networks ===");
        for (uint32_t i = 0; i < network_count; i++) {
            consciousness_dump_network((int32_t)i);
        }
    }
}

size_t consciousness_get_stats_string(char *buffer, size_t size) {
    if (!buffer || size == 0)
        return 0;

    /*
     * Write basic stats into buffer.
     * Since we have no snprintf in freestanding kernel,
     * we construct a simple fixed string and count valid CPCBs.
     */
    const char *header = "CONSCIOUSNESS STATS: ";
    size_t pos = 0;

    /* Copy header */
    size_t hlen = 0;
    while (header[hlen] != '\0')
        hlen++;
    if (hlen > size - 1)
        hlen = size - 1;
    for (size_t i = 0; i < hlen; i++) {
        buffer[pos++] = header[i];
    }

    /* Count active CPCBs */
    uint32_t active_count = 0;
    uint32_t verified_count = 0;
    for (uint32_t i = 0; i < MAX_RESONANT_PROCESSES; i++) {
        if (cpcb_table[i].magic == CPCB_MAGIC) {
            active_count++;
            if (cpcb_table[i].is_verified) {
                verified_count++;
            }
        }
    }

    /* Write active count as decimal digits */
    if (pos < size - 1)
        buffer[pos++] = 'A';
    if (pos < size - 1)
        buffer[pos++] = '=';

    /* Simple decimal conversion for active_count */
    char digits[12];
    uint32_t dlen = 0;
    uint32_t val = active_count;
    if (val == 0) {
        digits[dlen++] = '0';
    } else {
        while (val > 0 && dlen < 11) {
            digits[dlen++] = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    /* Reverse digits into buffer */
    for (uint32_t i = 0; i < dlen && pos < size - 1; i++) {
        buffer[pos++] = digits[dlen - 1 - i];
    }

    if (pos < size - 1)
        buffer[pos++] = ' ';

    /* Write verified count */
    if (pos < size - 1)
        buffer[pos++] = 'V';
    if (pos < size - 1)
        buffer[pos++] = '=';

    dlen = 0;
    val = verified_count;
    if (val == 0) {
        digits[dlen++] = '0';
    } else {
        while (val > 0 && dlen < 11) {
            digits[dlen++] = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    for (uint32_t i = 0; i < dlen && pos < size - 1; i++) {
        buffer[pos++] = digits[dlen - 1 - i];
    }

    if (pos < size - 1)
        buffer[pos++] = ' ';

    /* Write network count */
    if (pos < size - 1)
        buffer[pos++] = 'N';
    if (pos < size - 1)
        buffer[pos++] = '=';

    dlen = 0;
    val = network_count;
    if (val == 0) {
        digits[dlen++] = '0';
    } else {
        while (val > 0 && dlen < 11) {
            digits[dlen++] = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    for (uint32_t i = 0; i < dlen && pos < size - 1; i++) {
        buffer[pos++] = digits[dlen - 1 - i];
    }

    /* Null terminate */
    buffer[pos] = '\0';

    return pos;
}
