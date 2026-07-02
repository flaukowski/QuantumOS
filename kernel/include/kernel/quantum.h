/**
 * QuantumOS Quantum Resource Management (#4)
 *
 * Qubits, coherence windows, and quantum execution contexts as
 * first-class, capability-guarded OS resources. The current backend is
 * a deterministic in-kernel simulator (integer-only: fidelity in
 * basis points, coherence in timer ticks, measurements from a seeded
 * xorshift PRNG); the API is written against the quantum_types.h
 * shapes so hardware backends slot in behind it later.
 *
 * Access control: every operation takes (pid, cap_id) and validates a
 * CAP_RESOURCE_QUANTUM capability on the shared pool via cap_check —
 * no ambient authority.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QUANTUM_H
#define QUANTUM_H

#include <kernel/types.h>
#include <kernel/quantum_types.h>
#include <kernel/capability.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define QUANTUM_MAX_QUBITS          64   /* simulated pool size */
#define QUANTUM_MAX_CONTEXTS        16
#define QUANTUM_MAX_CTX_QUBITS      16   /* qubits per context */
#define QUANTUM_POOL_RESOURCE_ID    0    /* resource_id of the shared pool */

/* Default coherence window for a fresh allocation, in timer ticks
 * (10 ms each at TIMER_DEFAULT_HZ) */
#define QUANTUM_COHERENCE_TICKS     500  /* 5 seconds */

/* Fidelity cost per gate, in basis points (fidelity is 0..10000) */
#define QUANTUM_GATE_FIDELITY_COST  2

/* ============================================================================
 * API
 * ============================================================================ */

/* Initialize the quantum resource manager (before process_init) */
quantum_result_t quantum_init(void);

/* Mint a capability for the shared qubit pool, owned by pid.
 * Kernel-side helper used at process creation for quantum-aware
 * processes; permissions should include CAP_QUANTUM. */
quantum_result_t quantum_grant(uint32_t pid, uint32_t permissions,
                               uint32_t *cap_id_out);

/* Allocate `count` qubits for pid. Requires CAP_QUANTUM | CAP_WRITE. */
quantum_result_t quantum_alloc_qubits(uint32_t pid, uint32_t cap_id,
                                      uint32_t count, uint32_t *qubit_ids_out);

/* Release qubits previously allocated by pid. */
quantum_result_t quantum_release_qubits(uint32_t pid, uint32_t cap_id,
                                        const uint32_t *qubit_ids, uint32_t count);

/* Remaining coherence window of a qubit in timer ticks (0 = decohered
 * or unallocated). Coherence is tracked lazily against the tick
 * counter — no per-tick bookkeeping. */
uint64_t quantum_coherence_remaining(uint32_t qubit_id);

/* Create an execution context with `qubits_required` freshly
 * allocated qubits. Requires CAP_QUANTUM | CAP_WRITE. */
quantum_result_t quantum_context_create(uint32_t pid, uint32_t cap_id,
                                        uint32_t qubits_required,
                                        uint32_t priority,
                                        uint32_t *context_id_out);

/* Destroy a context and release its qubits. */
quantum_result_t quantum_context_destroy(uint32_t pid, uint32_t cap_id,
                                         uint32_t context_id);

/* Execute a gate sequence on a context (simulated). Gates target
 * context-relative qubit indices. GATE_MEASURE appends a
 * measurement_event_t to results. Requires CAP_QUANTUM | CAP_EXECUTE.
 * Fails with QUANTUM_ERROR_DECOHERED if any context qubit's coherence
 * window has closed. */
quantum_result_t quantum_execute_circuit(uint32_t pid, uint32_t cap_id,
                                         uint32_t context_id,
                                         const quantum_gate_t *gates,
                                         uint32_t num_gates,
                                         measurement_event_t *results,
                                         uint32_t max_results,
                                         uint32_t *num_results_out);

/* Pool statistics */
void quantum_get_pool_stats(qubit_pool_t *stats);

/* Boot-time self test; logs results, QUANTUM_SUCCESS if all pass */
quantum_result_t quantum_selftest(void);

#endif /* QUANTUM_H */
