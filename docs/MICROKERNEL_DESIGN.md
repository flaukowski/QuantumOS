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

### Shared-table atomicity (single-CPU invariant)

QuantumOS is single-CPU, and syscalls enter through an **interrupt gate**, so
they run `cli`'d (`IF=0`) and are atomic with respect to interrupts and to each
other. Kernel data structures a syscall mutates — the PMM frame bitmap and
free/used counters, the kernel-heap free-list, the capability table, the IPC
free-list, the QPU job table, the scheduler `ready_queue[]` — rely on this: a
`cli`'d syscall reads-modifies-writes them without its own lock.

The one thing that breaks the assumption is a mutator that reaches those same
tables with **interrupts enabled (`IF=1`)**. Two paths do:

- the **idle-loop reaper** (`process_reap`), which the idle loop deliberately
  runs inside `interrupt_disable_all()` — so it is safe; and
- the **health-monitor thread** (`service.c`), a `PRIORITY_HIGH` kernel thread
  that runs at `IF=1` and, on a missed heartbeat, calls
  `service_restart → service_stop → process_destroy`.

That second path is the recurring hazard: a timer IRQ can preempt it mid-update,
switch to a thread that enters a `cli`'d `SYS_SPAWN`, and let that spawn mutate
the very table the monitor had half-updated — losing a write. The invariant is
therefore: **every `IF=1` mutator of a shared table must bracket its critical
section `cli`'d**, or delegate to a table that self-brackets. Concretely:

- `pmm_alloc_frame` / `pmm_free_frame` bracket their bitmap+counter RMW in
  `irq_save()/irq_restore()`, exactly as `kmalloc`/`kfree` already do (a lost
  bitmap update would mark an in-use frame free and re-hand-out one physical
  frame to two address spaces).
- `service_stop` brackets its whole generation-guard → `process_set_state` →
  `process_destroy` → slot retire in `svc_irq_save()/svc_irq_restore()`
  (mirroring `start_slot`'s spawn-half bracket). Without it the generation check
  is a TOCTOU that can destroy an innocent recycled pid, and the `ready_queue[]`
  unlink races the timer scheduler into a use-after-free of the reaped PCB.
- `cap_revoke_all_for_process` (capability table) and `manifest_bind`
  (manifest table) already self-bracket, so the `process_destroy` they run
  under is safe on those tables.

### ELF loader validation

`elf_load` runs inside the `cli`'d `SYS_SPAWN` and loads an `ET_EXEC` image from
the (build-time trusted) initrd, but validates defensively so a malformed image
can neither leak kernel memory nor corrupt the address space:

- **Overflow-safe bounds.** The program-header table check
  (`e_phoff + e_phnum*e_phentsize`) and each segment's file-extent check
  (`p_offset + p_filesz`) are written as `a > size || b > size - a` so an
  attacker-shaped `e_phoff`/`p_offset` near `2^64` cannot wrap the sum below
  `size` and pass — the old additive form did, then read kernel memory far
  outside the image into a user-mapped page.
- **User-half confinement.** Every `PT_LOAD` segment must satisfy
  `USER_VBASE ≤ p_vaddr` and `p_vaddr + p_memsz ≤ 0x80000000` (no wrap), with
  `p_filesz ≤ p_memsz`. A `p_vaddr` below `USER_VBASE` would otherwise map
  through the *shared* boot page directory's 2 MB `PS` pages — `vmspace_map_page`
  would misread a 2 MB frame as a page table and corrupt physical memory.
- **No frame leak on error.** *Every* spawn error path that has already built
  (part of) a private address space reclaims it with `vmspace_destroy` before
  returning — `load_segment` frees a frame it allocated but could not map,
  `map_fresh_page` frees its unmapped frame, and `spawn_elf_args`,
  `user_process_spawn`, and `finalize_user_process` each `vmspace_destroy` on
  their OOM / `process_create`-failure paths (up until the address space is
  *bound* to a PCB, after which `process_destroy` owns it). Otherwise a spawn
  leaks its whole private half + page tables per failed attempt: spawning a
  non-ELF initrd file from qsh leaked on the `elf_load` path, and — once the
  256-slot process table is full — *every* further `run` leaks an address space
  on the `process_create`-failure path, a ring-3-reachable pmm-exhaustion DoS.
  A boot self-test (`spawn_leak_selftest`, using a one-shot `process_create`
  fault-injection seam) drives the create-failure path and asserts the pmm
  free-frame count is unchanged, gating on `SPAWNLEAK: failed spawn reclaimed
  address space (no leak)`.

A kernel boot self-test (`elf_spawn_selftest`) drives all three rejection paths
every boot and asserts the pmm free-frame count is unchanged, emitting
`ELFGUARD: malformed spawn rejected, no frame leak`; the CI smoke gate greps for
it, and without the fixes the self-test panics the boot before it prints.

### Boot-entropy integrity and serial liveness

Two more untrusted-input / liveness hardenings from the adversarial bug-hunt:

- **The `qseed=` boot token cannot zero the PRNG.** `xorshift64` has `0` as an
  *absorbing* fixed point — a zero state returns `0` forever. Because the
  SplitMix64 avalanche that mixes the boot qseed into the state is a bijection,
  an attacker who controls the untrusted `qseed=` cmdline token can invert it
  and pick the one value whose avalanche equals the golden init constant,
  cancelling the state to exactly `0` and silently, permanently disabling *all*
  kernel randomness — `SYS_QRAND` (returning zero bytes while still reporting
  provenance OK), `quantum_kernel_rand` (which the DNS txid/source-port
  anti-spoof depends on), and any service's Lamport key material. `quantum.c`
  now mixes through `mix_seed_nonzero`, which never leaves the state at `0`. The
  quantum boot self-test asserts this for both the specific attacker value and
  the generic cancellation, gating on `QSEEDGUARD: PRNG survives adversarial
  qseed (non-zero)`.
- **The COM2 swarm-bridge write cannot hang the kernel.** `com2_write` runs
  `cli`'d from `SYS_COM2`; an unbounded THR-drain spin on a wedged port — a
  swarm peer that stops draining the serial link — would freeze the *entire*
  kernel (no timer tick, no preemption), not just the writer. It now caps the
  per-byte wait (`COM2_THR_DRAIN_SPINS`) and latches a sticky `com2_dead`,
  exactly as `console.c` already does for the COM1 console, trading a dropped
  byte on a genuinely stuck port for guaranteed liveness.

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

### User-Memory Safety (copy in/out)

Syscalls run on an interrupt gate with `IF=0`, in ring 0, in the **caller's**
address space (its PML4 is the live CR3). Any user pointer a syscall
dereferences to copy arguments in or results out must therefore be validated
against the caller's page tables *before* the kernel touches it — a fault in
this context is a ring-0 fault, and `contain_user_fault` treats `(cs&3)==0` as
fatal and `boot_panic`s. A pointer that is merely inside the user address
*range* is not enough: a process maps only its code window plus a sparse stack
within the 1 GB user half, so an in-range-but-unmapped pointer would fault and
halt the whole machine (issue #158, a ring-3 → whole-OS DoS).

Every copy is gated by `vmspace_user_ok(pml4, uvaddr, len, need_write)`
(`kernel/src/vmspace.c`), reached through the `user_ok()` /
`copy_user_string()` helpers in `kernel/src/syscall.c`:

- It walks PML4 → PDPT → PD → PT for each 4 KB page the span touches,
  requiring `PG_PRESENT` at every level and `PG_PRESENT|PG_USER` (plus `PG_RW`
  when `need_write`) at the leaf. Any miss returns `false` → the syscall
  returns `SYSCALL_EFAULT` instead of faulting.
- The span is first confined to `[USER_VBASE, 0x80000000)` (PDPT[1], all 4 KB
  pages). This deliberately excludes the shared kernel identity map below
  `USER_VBASE` — walking those supervisor 2 MB `PS` pages as if they were page
  tables could falsely pass and hand ring 3 arbitrary kernel R/W, an escalation
  strictly worse than the DoS it guards.
- Copy-**in** validates with `need_write=0`; copy-**out** with `need_write=1`,
  so a write into a mapped-but-read-only page (e.g. the R-X code segment) is
  refused rather than faulting. Variable-length string copies
  (`copy_user_string`) validate each page just before crossing into it, so they
  never over-read past a mapped-region boundary.

The `ghost_test` boot citizen proves both failure modes by attack every boot
(`COPYGUARD: unmapped pointer denied` and `COPYGUARD: RO copy-out denied`): a
copy-out to an unmapped in-range pointer and to its own read-only code page must
each return `EFAULT`. Without the guard these calls fault in ring 0 and
`boot_panic` halts the boot, so the CI smoke gate — which greps for both lines
*and* every later boot gate — can never pass vacuously.

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

## Trust-core hardening (adversarial bug-hunt)

An adversarial sweep of the trust core (capability / IPC / manifest / audit)
fixed six defects; a seventh (a ring-3 kernel-panic via an unmapped-but-in-range
pointer) is tracked as a dedicated follow-up (`copy_from_user` page-table
validation).

- **Shared-table IF=1 races.** The capability table (`free_slot`/`alloc_slot`)
  and the IPC message-entry free list mutated shared state without masking
  interrupts, while `process_destroy` reaches them at IF=1 from the service
  health-monitor thread. A timer preemption mid-mutation racing a cli'd syscall
  could corrupt the table (leaked/double-allocated slot, skewed accounting).
  Both now bracket their mutators cli'd, matching every sibling subsystem.
- **IPC share-grant duplicate bypass.** A re-grant to a grantee whose active
  slot sat after a freed hole was created twice (double `ref_count`, a grant
  left behind on revoke); the scan now checks all slots before reusing a hole.
- **Audit ledger dump.** `audit_format` now emits whole lines only (never a
  clipped mid-field value) with a `truncated=1` marker, mirroring the manifest
  dump; `audit_load` rejects an implausibly large restored event count that
  would wrap the sequence counter.
- **IPC receive.** `SYS_RECV` validates the destination span *before* dequeuing,
  so a bad buffer no longer silently consumes-and-loses a message.
