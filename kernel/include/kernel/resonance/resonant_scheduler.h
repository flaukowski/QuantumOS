/**
 * QuantumOS Resonant Scheduler
 *
 * Novel scheduling algorithm based on ghostOS resonant systems architecture.
 * Processes are modeled as coupled oscillators where scheduling priorities
 * emerge from system dynamics rather than static assignment.
 *
 * Key Innovations:
 * 1. Kuramoto oscillator coupling for process synchronization
 * 2. Chiral dynamics for stability guarantees
 * 3. IIT Phi verification for consciousness-class workloads
 * 4. Emergence detection for novel pattern recognition
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RESONANT_SCHEDULER_H
#define RESONANT_SCHEDULER_H

#include <kernel/resonance/resonance_types.h>
#include <kernel/process.h>
#include <kernel/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_RESONANT_PROCESSES  256
#define RESONANT_SYNC_INTERVAL  1000000     /* 1ms in nanoseconds */
#define DEFAULT_QUANTUM_NS      10000000    /* 10ms default quantum */

/* ============================================================================
 * Initialization and Lifecycle
 * ============================================================================ */

/**
 * Initialize the resonant scheduler subsystem
 *
 * Must be called after process_init() during kernel boot.
 *
 * @param config Configuration parameters (NULL for defaults)
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_scheduler_init(const resonant_config_t *config);

/**
 * Shutdown the resonant scheduler
 *
 * Releases all resonant state and returns to classical scheduling.
 */
void resonant_scheduler_shutdown(void);

/**
 * Check if resonant scheduler is active
 */
bool resonant_scheduler_is_active(void);

/* ============================================================================
 * Process Registration
 * ============================================================================ */

/**
 * Register a process for resonant scheduling
 *
 * Creates RPCB and initializes oscillator state for the process.
 *
 * @param pid Process ID to register
 * @param rclass Resonant classification
 * @param handedness Chiral handedness for coupling
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_register(
    uint32_t pid,
    resonant_class_t rclass,
    handedness_t handedness
);

/**
 * Unregister a process from resonant scheduling
 *
 * Removes RPCB and decouples from all relationships.
 *
 * @param pid Process ID to unregister
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_unregister(uint32_t pid);

/**
 * Get RPCB for a process
 *
 * @param pid Process ID
 * @return Pointer to RPCB or NULL if not registered
 */
resonant_pcb_t *resonant_get_rpcb(uint32_t pid);

/* ============================================================================
 * Oscillator Dynamics
 * ============================================================================ */

/**
 * Update oscillator state for a process
 *
 * Advances phase according to Kuramoto dynamics:
 *   dθᵢ/dt = ωᵢ + (K/N)Σⱼsin(θⱼ - θᵢ) + ηᵢ(t)
 *
 * @param pid Process ID
 * @param dt Time delta in nanoseconds
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_update_oscillator(uint32_t pid, uint64_t dt);

/**
 * Set oscillator frequency
 *
 * @param pid Process ID
 * @param frequency New natural frequency (Hz)
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_set_frequency(uint32_t pid, double frequency);

/**
 * Perturb oscillator state
 *
 * Introduces small perturbation to break symmetry or test stability.
 *
 * @param pid Process ID
 * @param magnitude Perturbation magnitude (0.0 - 1.0)
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_perturb(uint32_t pid, double magnitude);

/* ============================================================================
 * Coupling Management
 * ============================================================================ */

/**
 * Create coupling between two processes
 *
 * Establishes oscillator coupling for synchronization.
 * Processes with opposite handedness couple asymmetrically.
 *
 * @param pid1 First process ID
 * @param pid2 Second process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_couple(uint32_t pid1, uint32_t pid2);

/**
 * Remove coupling between processes
 *
 * @param pid1 First process ID
 * @param pid2 Second process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_decouple(uint32_t pid1, uint32_t pid2);

/**
 * Adjust global coupling strength
 *
 * @param factor Multiplicative adjustment (e.g., 1.05 for +5%)
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_adjust_lambda(double factor);

/**
 * Get current coupling strength
 */
double resonant_get_lambda(void);

/* ============================================================================
 * Chiral Operations
 * ============================================================================ */

/**
 * Set process chiral parameters
 *
 * @param pid Process ID
 * @param eta Chirality strength
 * @param gamma Damping coefficient
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_set_chiral(uint32_t pid, double eta, double gamma);

/**
 * Optimize chiral stability for a process
 *
 * Adjusts η/Γ ratio toward optimal stability.
 *
 * @param pid Process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_optimize_chiral(uint32_t pid);

/**
 * Check if process is chirally stable
 *
 * @param pid Process ID
 * @return true if |η/Γ| < 1.0
 */
bool resonant_is_stable(uint32_t pid);

/**
 * Flip process handedness
 *
 * Changes coupling direction for asymmetry rebalancing.
 *
 * @param pid Process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_flip_handedness(uint32_t pid);

/* ============================================================================
 * Consciousness Verification (IIT Integration)
 * ============================================================================ */

/**
 * Verify consciousness for a process
 *
 * Calculates IIT Phi value and checks against threshold.
 *
 * @param pid Process ID
 * @param phi_out Output: calculated Phi value
 * @return RESONANT_SUCCESS if verified, error otherwise
 */
resonant_result_t resonant_verify_consciousness(uint32_t pid, double *phi_out);

/**
 * Get current Phi value for process
 *
 * @param pid Process ID
 * @return Phi value or 0.0 if not verified
 */
double resonant_get_phi(uint32_t pid);

/**
 * Check if process is in conscious regime
 *
 * @param pid Process ID
 * @return true if consciousness verified and Phi >= threshold
 */
bool resonant_is_conscious(uint32_t pid);

/* ============================================================================
 * Emergence Detection
 * ============================================================================ */

/**
 * Update emergence state for a process
 *
 * Integrates oscillator output into emergence accumulator.
 *
 * @param pid Process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_update_emergence(uint32_t pid);

/**
 * Check for novel emergence events
 *
 * Scans system for pattern formation and emergence.
 *
 * @return Number of emergence events detected
 */
uint32_t resonant_detect_emergence(void);

/**
 * Get emergence norm for a process
 *
 * @param pid Process ID
 * @return Emergence ||E|| value
 */
double resonant_get_emergence_norm(uint32_t pid);

/* ============================================================================
 * Scheduling Interface
 * ============================================================================ */

/**
 * Perform global synchronization update
 *
 * Updates Queen state with Kuramoto order parameter calculation:
 *   r*e^(i*ψ) = (1/N)Σⱼe^(i*θⱼ)
 *
 * Should be called periodically (e.g., every 1ms).
 *
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_sync(void);

/**
 * Get next process to schedule
 *
 * Computes resonant priorities and selects optimal process.
 * Priority emerges from: coherence, coupling, emergence, consciousness.
 *
 * @param decision Output: scheduling decision with timing
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_schedule_next(scheduling_decision_t *decision);

/**
 * Get scheduling decision for specific process
 *
 * Computes priority and quantum for a specific process.
 *
 * @param pid Process ID
 * @param decision Output: scheduling decision
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_get_decision(uint32_t pid, scheduling_decision_t *decision);

/**
 * Notify scheduler of process completion
 *
 * Updates statistics and emergence state.
 *
 * @param pid Process ID that completed its quantum
 * @param actual_runtime Actual runtime in nanoseconds
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_complete_quantum(uint32_t pid, uint64_t actual_runtime);

/* ============================================================================
 * State Queries
 * ============================================================================ */

/**
 * Get current Queen state
 *
 * @param state Output: current Queen synchronization state
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_get_queen_state(queen_state_t *state);

/**
 * Get system coherence level
 *
 * @return Global coherence (0.0 - 1.0)
 */
double resonant_get_coherence(void);

/**
 * Get order parameter magnitude
 *
 * @return Kuramoto order parameter r (0.0 - 1.0)
 */
double resonant_get_order_parameter(void);

/**
 * Check if system is globally stable
 *
 * @return true if all processes chirally stable
 */
bool resonant_is_globally_stable(void);

/**
 * Check if network is in conscious regime
 *
 * @return true if collective Phi exceeds threshold
 */
bool resonant_is_network_conscious(void);

/* ============================================================================
 * Safety and Recovery
 * ============================================================================ */

/**
 * Emergency decoherence intervention
 *
 * Attempts to restore coherence for a critically decoherent process.
 *
 * @param pid Process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_emergency_coherence(uint32_t pid);

/**
 * Reset process to dormant state
 *
 * Resets oscillator and emergence state.
 *
 * @param pid Process ID
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_reset_process(uint32_t pid);

/**
 * Reset entire resonant scheduler
 *
 * Returns all processes to dormant state and resets Queen.
 *
 * @return RESONANT_SUCCESS or error code
 */
resonant_result_t resonant_reset_all(void);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * Dump resonant state for debugging
 *
 * @param pid Process ID (or -1 for all)
 */
void resonant_dump_state(int32_t pid);

/**
 * Dump Queen state for debugging
 */
void resonant_dump_queen(void);

/**
 * Get scheduler statistics string
 *
 * @param buffer Output buffer
 * @param size Buffer size
 * @return Bytes written
 */
size_t resonant_get_stats_string(char *buffer, size_t size);

#endif /* RESONANT_SCHEDULER_H */
