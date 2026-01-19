# QuantumOS Process Management System

## Overview

The Process Management System is a core component of the QuantumOS microkernel that provides process lifecycle management, scheduling, and integration with the IPC system. This implementation addresses **Issue #1** from the QuantumOS project.

## Features

### Core Functionality
- **Process Creation and Destruction** - Full lifecycle management
- **Process State Management** - Ready, running, blocked, terminated states
- **Priority-based Scheduling** - 6 priority levels from idle to kernel
- **Process Relationships** - Parent-child process tracking
- **IPC Integration** - Each process has an associated message queue

### Quantum-Aware Features
- **Quantum Process Types** - Support for quantum-aware processes
- **Qubit Allocation** - Track qubit allocation per process
- **Quantum Runtime Tracking** - Monitor quantum operation time

### Security Features
- **Capability Integration** - Foundation for capability-based security
- **Process Isolation** - Memory protection and isolation
- **Privilege Levels** - Kernel, service, and user process types

## Architecture

### Process Control Block (PCB)

The core data structure is the `process_t` which contains:

```c
typedef struct process {
    // Basic information
    uint32_t pid;                  // Process ID
    uint32_t parent_pid;           // Parent process ID
    char name[PROCESS_NAME_MAX_LEN]; // Process name
    process_type_t type;           // Process type
    process_state_t state;         // Current state
    uint8_t priority;              // Process priority
    
    // Execution context
    uint64_t rip;                  // Instruction pointer
    uint64_t rsp;                  // Stack pointer
    uint64_t rbp;                  // Base pointer
    uint64_t cr3;                  // Page table physical address
    
    // Memory management
    void *virtual_address_space;   // Virtual memory root
    size_t memory_size;            // Total memory allocated
    void *stack_top;               // Top of kernel stack
    
    // Timing and scheduling
    uint64_t creation_time;        // Creation timestamp
    uint64_t runtime_total;        // Total runtime
    uint64_t last_scheduled;       // Last scheduling time
    
    // IPC integration
    uint32_t message_queue_id;     // IPC message queue ID
    
    // Quantum support
    struct {
        bool is_quantum_aware;     // Can use quantum resources
        uint32_t qubit_allocation; // Allocated qubits
        uint64_t quantum_runtime;  // Quantum operation time
    } quantum;
    
    // ... additional fields
} process_t;
```

### Process States

Processes can be in one of these states:

- **PROCESS_STATE_UNUSED** - Slot not used
- **PROCESS_STATE_CREATED** - Created but not runnable
- **PROCESS_STATE_READY** - Ready to run
- **PROCESS_STATE_RUNNING** - Currently running
- **PROCESS_STATE_BLOCKED** - Blocked (waiting for I/O, etc.)
- **PROCESS_STATE_TERMINATED** - Terminated but not cleaned up
- **PROCESS_STATE_ZOMBIE** - Terminated, waiting for parent

### Process Types

- **PROCESS_TYPE_KERNEL** - Kernel processes (highest privilege)
- **PROCESS_TYPE_USER** - Regular user processes
- **PROCESS_TYPE_SERVICE** - System services
- **PROCESS_TYPE_QUANTUM** - Quantum-aware processes

### Priority Levels

- **PRIORITY_IDLE** (0) - Lowest priority
- **PRIORITY_LOW** (1) - Low priority
- **PRIORITY_NORMAL** (2) - Normal priority
- **PRIORITY_HIGH** (3) - High priority
- **PRIORITY_REALTIME** (4) - Real-time priority
- **PRIORITY_KERNEL** (5) - Kernel priority (highest)

## API Reference

### Initialization

```c
status_t process_init(void);
```
Initialize the process management system. Creates kernel and idle processes.

### Process Creation

```c
status_t process_create(const process_create_params_t *params, process_t **process);
```
Create a new process with the specified parameters.

### Process Destruction

```c
status_t process_destroy(uint32_t pid);
status_t process_exit(uint32_t pid, int32_t exit_code);
status_t process_kill(uint32_t pid, int32_t signal);
```
Destroy or terminate a process.

### State Management

```c
status_t process_set_state(uint32_t pid, process_state_t new_state);
process_state_t process_get_state(uint32_t pid);
status_t process_block(uint32_t pid);
status_t process_unblock(uint32_t pid);
```
Manage process states.

### Process Information

```c
process_t *process_get_by_pid(uint32_t pid);
process_t *process_get_current(void);
uint32_t process_get_pid(process_t *process);
const char *process_get_name(uint32_t pid);
```
Get process information.

### Scheduling

```c
status_t process_schedule_next(void);
status_t process_switch_to(process_t *process);
process_t *process_get_next_ready(void);
```
Scheduling operations.

### Quantum Support

```c
status_t process_set_quantum_aware(uint32_t pid, bool aware);
bool process_is_quantum_aware(uint32_t pid);
status_t process_allocate_qubits(uint32_t pid, uint32_t count);
status_t process_deallocate_qubits(uint32_t pid, uint32_t count);
```
Quantum-aware process operations.

## Integration Points

### IPC System Integration

Each process is automatically assigned an IPC message queue upon creation:

```c
// During process creation - IPC manages queues internally by PID
ipc_result_t ipc_result = ipc_process_init(pid);
```

The process management system integrates with IPC for:
- Message queue creation/destruction (via `ipc_process_init`/`ipc_process_cleanup`)
- Process-to-process communication
- Service discovery

### Memory Management Integration

Process creation involves:
- Virtual address space setup
- Stack allocation
- Page table management
- Memory protection

### Interrupt System Integration

Process scheduling is triggered by:
- Timer interrupts for preemptive scheduling
- I/O completion interrupts
- System calls from user processes

## Usage Examples

### Creating a User Process

```c
process_create_params_t params = {
    .name = "my_process",
    .type = PROCESS_TYPE_USER,
    .priority = PRIORITY_NORMAL,
    .parent_pid = KERNEL_PROCESS_ID,
    .entry_point = (void*)my_process_main,
    .stack_address = (void*)0x400000,
    .stack_size = PROCESS_STACK_SIZE,
    .is_quantum_aware = false
};

process_t *process = NULL;
status_t result = process_create(&params, &process);
if (result == STATUS_SUCCESS) {
    boot_log("Created process %s with PID %d", process->name, process->pid);
}
```

### Creating a Quantum-Aware Process

```c
process_create_params_t params = {
    .name = "quantum_worker",
    .type = PROCESS_TYPE_QUANTUM,
    .priority = PRIORITY_HIGH,
    .parent_pid = KERNEL_PROCESS_ID,
    .entry_point = (void*)quantum_worker_main,
    .stack_address = (void*)0x500000,
    .stack_size = PROCESS_STACK_SIZE,
    .is_quantum_aware = true
};

process_t *process = NULL;
status_t result = process_create(&params, &process);
if (result == STATUS_SUCCESS) {
    // Allocate qubits for quantum operations
    process_allocate_qubits(process->pid, 16);
}
```

### Process State Management

```c
// Block a process waiting for I/O
status_t result = process_block(process_pid);
if (result == STATUS_SUCCESS) {
    // Process is now blocked and won't be scheduled
}

// Unblock when I/O completes
result = process_unblock(process_pid);
if (result == STATUS_SUCCESS) {
    // Process is back to ready state
}
```

## Implementation Details

### Scheduling Algorithm

The current implementation uses a simple priority-based round-robin scheduler:

1. Find highest priority ready process
2. Switch to that process
3. Update timing statistics
4. Handle quantum expiration (not yet implemented)

### Memory Layout

Each process has:
- Kernel stack (8KB default)
- User virtual address space
- Page tables for memory protection
- IPC message queue

### Error Handling

The system uses standard status codes:
- `STATUS_SUCCESS` - Operation succeeded
- `STATUS_INVALID_ARG` - Invalid parameters
- `STATUS_NO_MEMORY` - Out of memory
- `PROCESS_ERROR_*` - Process-specific errors

## Testing

### Unit Tests

Comprehensive unit tests are provided in `tests/unit/test_process.c`:

- Process creation and destruction
- State management
- Process relationships
- Quantum-aware features
- Statistics tracking

### Integration Tests

Integration tests verify:
- IPC system interaction
- Memory management integration
- Scheduling behavior
- System boot sequence

### Running Tests

```bash
# Run all tests
make test

# Run process-specific tests
make test-process
```

## Performance Considerations

### Current Limitations

- No preemptive scheduling (timer interrupts not implemented)
- Simple round-robin within priority levels
- No dynamic priority adjustment
- No process migration between CPUs

### Future Optimizations

- Implement preemptive scheduling
- Add multi-CPU support
- Implement fair scheduling algorithms
- Add process priority inheritance
- Optimize context switch time

## Security Considerations

### Current Security Features

- Process isolation through memory protection
- Capability system foundation
- Privilege level separation
- Resource limits (maximum processes)

### Future Security Enhancements

- Complete capability-based security
- Process sandboxing
- Resource quotas
- Audit logging
- Secure process communication

## Troubleshooting

### Common Issues

1. **Process Creation Fails**
   - Check if maximum process limit reached
   - Verify memory allocation
   - Check parent process validity

2. **Scheduling Issues**
   - Verify process states
   - Check ready queue integrity
   - Validate process priorities

3. **Memory Issues**
   - Check stack allocation
   - Verify page table setup
   - Check memory protection

### Debug Functions

Use these functions for debugging:

```c
void process_dump_info(uint32_t pid);      // Dump single process
void process_dump_all(void);               // Dump all processes
void process_dump_scheduler_queue(void);   // Dump scheduling queues
status_t process_get_stats(process_stats_t *stats); // Get statistics
```

## Future Development

### Phase 0.3 Planned Features

- Preemptive scheduling with timer interrupts
- Multi-CPU support
- Advanced scheduling algorithms
- Process resource limits
- Complete capability system integration

### Phase 1.0 Target Features

- Full microkernel process management
- Advanced security features
- Performance optimization
- Comprehensive testing suite
- Production-ready reliability

## Contributing

When contributing to the process management system:

1. Follow the coding standards in `CONTRIBUTING.md`
2. Add comprehensive tests for new features
3. Update documentation
4. Ensure integration with existing systems
5. Test on all supported architectures

## License

This implementation is licensed under GPL v2.0, consistent with the QuantumOS project license.
