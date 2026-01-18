#ifndef QUANTUM_TYPES_H
#define QUANTUM_TYPES_H

#include <stdint.h>
#include <stddef.h>

// Quantum result codes
typedef enum {
    QUANTUM_SUCCESS = 0,
    QUANTUM_ERROR_NO_QUBITS = -1,
    QUANTUM_ERROR_DECOHERED = -2,
    QUANTUM_ERROR_MEASUREMENT_FAILED = -3,
    QUANTUM_ERROR_HARDWARE_FAULT = -4,
    QUANTUM_ERROR_INSUFFICIENT_COHERENCE = -5,
    QUANTUM_ERROR_CIRCUIT_TOO_DEEP = -6
} quantum_result_t;

// Process types for hybrid execution
typedef enum {
    PROCESS_CLASSICAL,
    PROCESS_QUANTUM,
    PROCESS_HYBRID,
    PROCESS_AGENT
} process_type_t;

// Physical or simulated qubit reference
typedef struct {
    uint32_t qubit_id;
    uint32_t simulator_id;
    uint64_t coherence_time;  // Remaining coherence window (ns)
    uint32_t fidelity;        // Current fidelity (0-10000 = 0.00-100.00%)
    uint8_t is_allocated;
} qubit_handle_t;

// Quantum program execution context
typedef struct {
    uint32_t context_id;
    uint32_t priority;
    uint64_t deadline;        // Coherence deadline (ns)
    uint32_t qubits_required;
    uint32_t *qubit_ids;
    uint32_t circuit_depth;
    uint8_t speculative;
} quantum_context_t;

// Quantum gate definition
typedef struct quantum_gate {
    uint32_t gate_type;
    uint32_t *target_qubits;
    uint32_t num_targets;
    uint32_t control_qubit;
    double parameter;
    uint64_t timestamp;
    struct quantum_gate *next;
} quantum_gate_t;

// Circuit graph (DAG of quantum operations)
typedef struct {
    uint32_t circuit_id;
    uint32_t num_gates;
    quantum_gate_t *gates;
    uint32_t depth;
    uint8_t is_measurement;
} circuit_graph_t;

// Coherence window management
typedef struct {
    uint64_t start_time;
    uint64_t duration;
    uint64_t remaining;
    uint32_t qubit_count;
    uint32_t *qubit_ids;
} coherence_window_t;

// Quantum measurement result
typedef struct {
    uint32_t measurement_id;
    uint32_t qubit_id;
    uint8_t result;           // 0 or 1
    double probability;
    uint64_t timestamp;
    uint8_t collapsed;
} measurement_event_t;

// Process requirements
typedef struct {
    process_type_t type;
    uint32_t cpu_cores_required;
    uint32_t qubits_required;
    uint64_t time_budget;
    double determinism_req;   // 0.0 = probabilistic, 1.0 = deterministic
    uint64_t uncertainty_bound;
} process_requirements_t;

// Qubit pool management
typedef struct {
    uint32_t total_qubits;
    uint32_t available_qubits;
    uint32_t allocated_qubits;
    uint32_t maintenance_qubits;
    uint32_t high_fidelity_qubits;
    uint32_t standard_qubits;
    uint32_t experimental_qubits;
} qubit_pool_t;

// Gate types
#define GATE_H        1
#define GATE_X        2
#define GATE_Y        3
#define GATE_Z        4
#define GATE_CNOT     5
#define GATE_CZ       6
#define GATE_RX       7
#define GATE_RY       8
#define GATE_RZ       9
#define GATE_MEASURE  10

// Fidelity thresholds
#define FIDELITY_HIGH    9990  // 99.90%
#define FIDELITY_STANDARD 9900  // 99.00%
#define FIDELITY_LOW      9500  // 95.00%

#endif // QUANTUM_TYPES_H
