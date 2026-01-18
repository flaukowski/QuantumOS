# Quantum Resource Model & Scheduler Architecture

## First-Class Quantum Objects

### Core Quantum Primitives
These are OS-level objects, not library abstractions.

```c
// Physical or simulated qubit reference
typedef struct {
    uint32_t qubit_id;         // Hardware qubit index
    uint32_t simulator_id;    // Simulator qubit index (if simulated)
    uint64_t coherence_time;  // Remaining coherence window (ns)
    uint32_t fidelity;        // Current fidelity (0-10000 = 0.00-100.00%)
    uint8_t is_allocated;     // Allocation status
} qubit_handle_t;

// Quantum program execution context
typedef struct {
    uint32_t context_id;
    uint32_t priority;        // Scheduling priority
    uint64_t deadline;        // Coherence deadline (ns)
    uint32_t qubits_required; // Number of qubits needed
    uint32_t *qubit_ids;      // Allocated qubits
    uint32_t circuit_depth;   // Circuit complexity metric
    uint8_t speculative;      // Can be speculatively executed
} quantum_context_t;

// Directed acyclic graph of quantum operations
typedef struct quantum_gate {
    uint32_t gate_type;       // H, X, Y, Z, CNOT, etc.
    uint32_t *target_qubits;  // Target qubit indices
    uint32_t num_targets;
    uint32_t control_qubit;   // For controlled gates (0 = none)
    double parameter;         // Rotation angle, etc.
    uint64_t timestamp;       // Logical execution time
} quantum_gate_t;

typedef struct {
    uint32_t circuit_id;
    uint32_t num_gates;
    quantum_gate_t *gates;
    uint32_t depth;           // Critical path depth
    uint8_t is_measurement;  // Ends with measurement?
} circuit_graph_t;

// Time budget before decoherence
typedef struct {
    uint64_t start_time;      // When window begins (ns)
    uint64_t duration;        // Window length (ns)
    uint64_t remaining;       // Time left (ns)
    uint32_t qubit_count;     // Qubits in this window
    uint32_t *qubit_ids;      // Affected qubits
} coherence_window_t;

// Quantum measurement result
typedef struct {
    uint32_t measurement_id;
    uint32_t qubit_id;
    uint8_t result;           // 0 or 1 (classical bit)
    double probability;       // Probability of this result
    uint64_t timestamp;       // When measurement occurred
    uint8_t collapsed;        // Did this cause collapse?
} measurement_event_t;
```

## Quantum Scheduler Architecture

### Scheduler Responsibilities
1. **Fair Qubit Allocation** - Prevent starvation, optimize utilization
2. **Coherence-Time Awareness** - Schedule within decoherence limits
3. **Circuit Batching** - Group compatible circuits for efficiency
4. **Speculative Execution** - Execute probabilistic branches
5. **Classical Integration** - Coordinate with classical scheduler

### Scheduling Policies (Pluggable)

#### FIFO Policy (Research Baseline)
```c
typedef struct {
    queue_t pending_contexts; // Simple FIFO queue
    uint32_t quantum_time_slice; // Fixed time quantum
} fifo_scheduler_t;
```

#### Coherence-Aware Priority
```c
typedef struct {
    heap_t priority_queue;   // Min-heap by deadline
    uint32_t urgency_threshold; // Boost near-deadline contexts
} coherence_scheduler_t;
```

#### Energy-Minimizing
```c
typedef struct {
    uint32_t idle_qubits;     // Track idle qubits
    uint32_t power_state;     // Current power mode
    uint64_t energy_budget;   // Energy constraints
} energy_scheduler_t;
```

#### Error-Rate Minimizing
```c
typedef struct {
    uint32_t qubit_fidelities; // Track qubit quality
    uint32_t error_threshold;  // Minimum acceptable fidelity
    uint32_t calibration_schedule; // When to recalibrate
} error_scheduler_t;
```

### Hybrid Classical-Quantum Scheduling

#### Process Types
```c
typedef enum {
    PROCESS_CLASSICAL,     // Pure classical computation
    PROCESS_QUANTUM,       // Pure quantum computation
    PROCESS_HYBRID,        // Mixed classical/quantum
    PROCESS_AGENT          // Long-lived autonomous agent
} process_type_t;

typedef struct {
    process_type_t type;
    uint32_t cpu_cores_required;
    uint32_t qubits_required;
    uint64_t time_budget;     // Total execution time budget
    double determinism_req;   // 0.0 = fully probabilistic, 1.0 = fully deterministic
    uint64_t uncertainty_bound; // Acceptable error margin
} process_requirements_t;
```

#### Scheduling Integration
```c
typedef struct {
    // Classical scheduler state
    scheduler_t classical_sched;
    
    // Quantum scheduler state
    quantum_scheduler_t quantum_sched;
    
    // Coordination state
    uint32_t active_hybrid_processes;
    uint64_t quantum_time_available;
    uint64_t classical_time_available;
} hybrid_scheduler_t;
```

## Resource Management

### Qubit Allocation Strategy
```c
typedef struct {
    uint32_t total_qubits;
    uint32_t available_qubits;
    uint32_t allocated_qubits;
    uint32_t maintenance_qubits; // Reserved for calibration
    
    // Qubit pools by characteristics
    uint32_t high_fidelity_qubits;   // >99.9% fidelity
    uint32_t standard_qubits;        // 99.0-99.9% fidelity
    uint32_t experimental_qubits;    // <99.0% fidelity
} qubit_pool_t;
```

### Coherence Window Management
```c
typedef struct {
    // Active coherence windows
    list_t active_windows;
    
    // Window prediction
    uint64_t avg_coherence_time;
    uint64_t decoherence_rate;
    
    // Window optimization
    uint32_t window_overlap_threshold;
    uint8_t allow_window_extension;
} coherence_manager_t;
```

## Implementation Structure

### Kernel Quantum Subsystem
```
kernel/quantum/
├── quantum_types.h        # Core quantum data structures
├── qubit_manager.c        # Qubit allocation/deallocation
├── context_manager.c      # Quantum context lifecycle
├── circuit_executor.c     # Circuit execution engine
├── coherence_tracker.c    # Coherence window management
├── measurement_handler.c  # Measurement processing
└── quantum_scheduler.c    # Main quantum scheduler
```

### User-Space Quantum Service
```
services/quantum-scheduler/
├── quantum_service.c      # Main service daemon
├── policy_engine.c       # Pluggable scheduling policies
├── resource_monitor.c    # Resource utilization tracking
├── error_recovery.c       # Quantum error handling
└── api/
    └── quantum_api.c      # IPC interface to kernel
```

### Hardware Integration
```
kernel/hal/quantum/
├── simulator_interface.c # Quantum simulator backend
├── hardware_interface.c   # Real quantum hardware interface
├── error_correction.c    # Quantum error correction
└── calibration.c          # Hardware calibration routines
```

## Error Handling & Recovery

### Quantum Error Types
```c
typedef enum {
    QUANTUM_SUCCESS = 0,
    QUANTUM_ERROR_NO_QUBITS = -1,
    QUANTUM_ERROR_DECOHERED = -2,
    QUANTUM_ERROR_MEASUREMENT_FAILED = -3,
    QUANTUM_ERROR_HARDWARE_FAULT = -4,
    QUANTUM_ERROR_INSUFFICIENT_COHERENCE = -5,
    QUANTUM_ERROR_CIRCUIT_TOO_DEEP = -6
} quantum_result_t;
```

### Recovery Strategies
1. **Graceful Degradation** - Use lower-fidelity qubits
2. **Circuit Recompilation** - Break deep circuits into shallow ones
3. **Speculative Retry** - Re-execute with different qubits
4. **Classical Fallback** - Switch to classical simulation

## Performance Metrics
- **Qubit Utilization** - Percentage of qubits in use
- **Coherence Efficiency** - Ratio of used coherence time to available
- **Circuit Throughput** - Circuits completed per second
- **Error Rate** - Quantum operation failure rate
- **Scheduling Latency** - Time from submission to execution

## Success Criteria
- [ ] Quantum objects are true OS primitives
- [ ] Scheduler respects coherence constraints
- [ ] Fair allocation across competing processes
- [ ] Seamless integration with classical scheduler
- [ ] Robust error handling and recovery
- [ ] Pluggable scheduling policies work correctly
