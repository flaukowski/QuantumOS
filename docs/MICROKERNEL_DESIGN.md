# Microkernel Core Design

## Core Responsibilities

The QuantumOS microkernel provides only what cannot be safely implemented in user space:

### Essential Kernel Functions
1. **Process & Thread Management** - Basic execution contexts
2. **Memory Protection** - Virtual memory and address spaces
3. **IPC System** - Message passing between processes
4. **Capability-Based Security** - Access control and permissions
5. **Hardware Interrupts** - IRQ handling and device communication
6. **Quantum Resource Arbitration** - Minimal quantum primitives only

## IPC System Architecture

### Message Passing Primitives

```c
// Message structure for IPC
typedef struct {
    uint32_t sender_id;
    uint32_t receiver_id;
    uint32_t message_type;
    uint32_t length;
    uint8_t data[4096];     // Max message size
    uint64_t timestamp;
} ipc_message_t;

// IPC result codes
typedef enum {
    IPC_SUCCESS = 0,
    IPC_ERROR_INVALID_RECEIVER = -1,
    IPC_ERROR_MESSAGE_TOO_LARGE = -2,
    IPC_ERROR_PERMISSION_DENIED = -3,
    IPC_ERROR_BUFFER_FULL = -4,
    IPC_ERROR_TIMEOUT = -5
} ipc_result_t;

// IPC operations
ipc_result_t ipc_send(uint32_t receiver, const ipc_message_t *msg);
ipc_result_t ipc_receive(uint32_t *sender, ipc_message_t *msg, uint64_t timeout_ns);
ipc_result_t ipc_reply(uint32_t receiver, const ipc_message_t *reply);
```

### Zero-Copy Optimization
```c
// Shared memory regions for large messages
typedef struct {
    void *address;
    size_t size;
    uint32_t permissions;
} ipc_shared_region_t;

ipc_result_t ipc_share_create(ipc_shared_region_t *region, size_t size);
ipc_result_t ipc_share_grant(uint32_t receiver, ipc_shared_region_t *region);
ipc_result_t ipc_share_revoke(uint32_t receiver, ipc_shared_region_t *region);
```

## Capability-Based Security

### Capability Structure
```c
// Capability token structure
typedef struct {
    uint32_t cap_id;           // Unique capability identifier
    uint32_t owner_id;         // Owning process
    uint32_t resource_id;       // Resource this cap controls
    uint32_t permissions;       // Permission bits
    uint64_t expiration;        // Expiration time (0 = never)
    uint8_t is_revocable;       // Can this be revoked?
} capability_t;

// Permission bits
#define CAP_READ    0x01
#define CAP_WRITE   0x02
#define CAP_EXECUTE 0x04
#define CAP_GRANT   0x08
#define CAP_REVOKE  0x10
#define CAP_QUANTUM 0x20  // Quantum resource access
```

### Capability Management
```c
// Capability operations
typedef enum {
    CAP_SUCCESS = 0,
    CAP_ERROR_NOT_FOUND = -1,
    CAP_ERROR_PERMISSION_DENIED = -2,
    CAP_ERROR_EXPIRED = -3,
    CAP_ERROR_REVOKED = -4
} cap_result_t;

cap_result_t capability_grant(uint32_t owner, uint32_t resource, uint32_t perms, capability_t **cap);
cap_result_t capability_revoke(capability_t *cap);
cap_result_t capability_check(const capability_t *cap, uint32_t required_perms);
cap_result_t capability_transfer(capability_t *cap, uint32_t new_owner);
```

## Memory Management

### Virtual Memory System
```c
// Page table entry structure (x86_64)
typedef struct {
    uint64_t present    : 1;
    uint64_t read_write : 1;
    uint64_t user       : 1;
    uint64_t pwt        : 1;  // Page-level write-through
    uint64_t pcd        : 1;  // Page-level cache disable
    uint64_t accessed   : 1;
    uint64_t dirty      : 1;
    uint64_t pat        : 1;  // Page-attribute table
    uint64_t global     : 1;
    uint64_t available  : 3;
    uint64_t frame      : 40; // Physical frame address
    uint64_t reserved   : 11;
    uint64_t nx         : 1;  // No-execute bit
} pte_t;

// Memory region
typedef struct {
    void *virtual_addr;
    void *physical_addr;
    size_t size;
    uint32_t permissions;
    uint8_t is_mapped;
} memory_region_t;
```

### Memory Operations
```c
typedef enum {
    MEM_SUCCESS = 0,
    MEM_ERROR_OUT_OF_MEMORY = -1,
    MEM_ERROR_INVALID_ADDRESS = -2,
    MEM_ERROR_ALIGNMENT = -3,
    MEM_ERROR_PERMISSION = -4
} mem_result_t;

mem_result_t memory_map(void *virt_addr, void *phys_addr, size_t size, uint32_t perms);
mem_result_t memory_unmap(void *virt_addr, size_t size);
mem_result_t memory_protect(void *virt_addr, size_t size, uint32_t perms);
mem_result_t memory_alloc(void **addr, size_t size, uint32_t perms);
```

## Process Management

### Process Structure
```c
// Process control block
typedef struct process {
    uint32_t pid;
    uint32_t parent_pid;
    char name[256];
    
    // Memory spaces
    pml4_t *page_table;
    memory_region_t *regions;
    size_t num_regions;
    
    // Capabilities
    capability_t *capabilities;
    size_t num_capabilities;
    
    // Execution state
    cpu_state_t *cpu_state;
    uint32_t state;  // RUNNING, BLOCKED, ZOMBIE
    
    // Scheduling
    uint32_t priority;
    uint64_t runtime;
    uint64_t last_run;
    
    // IPC
    queue_t message_queue;
    uint32_t message_queue_size;
    
    struct process *next;
    struct process *prev;
} process_t;

// Process states
#define PROCESS_RUNNING    0
#define PROCESS_BLOCKED    1
#define PROCESS_ZOMBIE     2
#define PROCESS_SLEEPING   3
```

### Process Operations
```c
typedef enum {
    PROC_SUCCESS = 0,
    PROC_ERROR_OUT_OF_MEMORY = -1,
    PROC_ERROR_INVALID_PID = -2,
    PROC_ERROR_PERMISSION_DENIED = -3,
    PROC_ERROR_ALREADY_EXISTS = -4
} proc_result_t;

proc_result_t process_create(const char *name, uint32_t parent_pid, process_t **proc);
proc_result_t process_destroy(uint32_t pid);
proc_result_t process_get(uint32_t pid, process_t **proc);
proc_result_t process_set_state(uint32_t pid, uint32_t state);
```

## Interrupt Handling

### Interrupt Descriptor Table
```c
// Interrupt gate descriptor
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;      // Interrupt stack table index
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

// CPU state saved on interrupt
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, rsp, ss;
} cpu_state_t;
```

### Interrupt Handlers
```c
// Interrupt handler function type
typedef void (*interrupt_handler_t)(cpu_state_t *state);

// Interrupt management
typedef enum {
    IRQ_SUCCESS = 0,
    IRQ_ERROR_INVALID_VECTOR = -1,
    IRQ_ERROR_ALREADY_REGISTERED = -2,
    IRQ_ERROR_PERMISSION_DENIED = -3
} irq_result_t;

irq_result_t irq_register(uint8_t vector, interrupt_handler_t handler);
irq_result_t irq_unregister(uint8_t vector);
irq_result_t irq_enable(uint8_t vector);
irq_result_t irq_disable(uint8_t vector);
```

## Quantum Resource Integration

### Minimal Quantum Primitives in Kernel
```c
// Quantum resource descriptor (kernel-managed)
typedef struct {
    uint32_t qubit_id;
    uint32_t owner_process;
    uint64_t coherence_deadline;
    uint32_t fidelity;
    uint8_t is_available;
} quantum_resource_t;

// Kernel quantum operations (minimal)
typedef enum {
    Q_SUCCESS = 0,
    Q_ERROR_NO_RESOURCES = -1,
    Q_ERROR_DECOHERED = -2,
    Q_ERROR_PERMISSION_DENIED = -3
} q_result_t;

q_result_t quantum_qubit_allocate(uint32_t process_id, quantum_resource_t **qubit);
q_result_t quantum_qubit_release(quantum_resource_t *qubit);
q_result_t quantum_coherence_check(quantum_resource_t *qubit, uint64_t *remaining_ns);
```

## System Call Interface

### System Call Numbers
```c
#define SYS_IPC_SEND        1
#define SYS_IPC_RECEIVE     2
#define SYS_CAP_GRANT       3
#define SYS_CAP_REVOKE      4
#define SYS_CAP_CHECK       5
#define SYS_MEM_MAP         6
#define SYS_MEM_UNMAP       7
#define SYS_PROC_CREATE     8
#define SYS_PROC_DESTROY    9
#define SYS_QUANTUM_ALLOC   10
#define SYS_QUANTUM_RELEASE 11
```

### System Call Handler
```c
// System call entry point
void syscall_handler(cpu_state_t *state);

// System call implementations
uint64_t sys_ipc_send(uint32_t receiver, const ipc_message_t *msg);
uint64_t sys_ipc_receive(uint32_t *sender, ipc_message_t *msg, uint64_t timeout);
uint64_t sys_capability_grant(uint32_t resource, uint32_t perms);
uint64_t sys_capability_revoke(uint32_t cap_id);
uint64_t sys_memory_map(void *virt, void *phys, size_t size, uint32_t perms);
uint64_t sys_process_create(const char *name);
uint64_t sys_quantum_allocate(quantum_resource_t **qubit);
```

## Implementation Structure

### Kernel Source Organization
```
kernel/
├── core/
│   ├── main.c              # Kernel entry point
│   ├── process.c           # Process management
│   ├── memory.c            # Virtual memory
│   ├── scheduler.c         # Basic scheduler
│   └── syscall.c           # System call dispatcher
├── ipc/
│   ├── message.c           # Message passing
│   ├── shared_memory.c     # Zero-copy regions
│   └── ipc_api.c           # IPC system calls
├── security/
│   ├── capability.c        # Capability management
│   ├── permissions.c       # Permission checking
│   └── security_api.c      # Security system calls
├── hal/
│   ├── interrupts.c        # Interrupt handling
│   ├── timer.c            # System timer
│   └── arch/              # Architecture-specific code
├── quantum/
│   ├── resources.c        # Quantum resource management
│   └── quantum_api.c      # Quantum system calls
└── include/
    ├── kernel.h            # Main kernel headers
    ├── process.h           # Process structures
    ├── memory.h            # Memory management
    ├── ipc.h              # IPC interfaces
    └── quantum.h          # Quantum primitives
```

## Success Criteria
- [ ] IPC system supports message passing between processes
- [ ] Capability system enforces access control
- [ ] Memory protection prevents unauthorized access
- [ ] Process isolation works correctly
- [ ] Interrupt handling is stable
- [ ] Quantum resource allocation/deallocation works
- [ ] System call interface is complete
- [ ] All components integrate seamlessly

## Performance Targets
- **IPC Latency**: < 100 microseconds for small messages
- **Capability Check**: < 1 microsecond
- **Memory Map**: < 10 microseconds for 4KB page
- **Context Switch**: < 5 microseconds
- **Interrupt Latency**: < 10 microseconds

This microkernel design provides the minimal foundation needed for QuantumOS while maintaining strict security boundaries and enabling the quantum-aware features defined in the PRD.
