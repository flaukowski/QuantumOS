/**
 * QuantumOS Process Management Unit Tests
 *
 * Unit tests for the process management system.
 * Tests process creation, destruction, state management,
 * and scheduling functionality.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/process.h>
#include <kernel/types.h>
#include <kernel/boot.h>

/* ============================================================================
 * Test Helper Functions
 * ============================================================================ */

static int test_count = 0;
static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        test_count++; \
        if (condition) { \
            test_passed++; \
            boot_log("[PASS] %s", message); \
        } else { \
            test_failed++; \
            boot_log("[FAIL] %s", message); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(expected, actual, message) \
    TEST_ASSERT((expected) == (actual), message)

#define TEST_ASSERT_NOT_NULL(ptr, message) \
    TEST_ASSERT((ptr) != NULL, message)

#define TEST_ASSERT_NULL(ptr, message) \
    TEST_ASSERT((ptr) == NULL, message)

/* Dummy test function for process entry point */
static void dummy_process_entry(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

/* ============================================================================
 * Test Cases
 * ============================================================================ */

/**
 * Test process system initialization
 */
static void test_process_init(void) {
    boot_log("Testing process system initialization...");
    
    status_t result = process_init();
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process system initialization");
    
    /* Check that kernel process exists */
    process_t *kernel_process = process_get_by_pid(KERNEL_PROCESS_ID);
    TEST_ASSERT_NOT_NULL(kernel_process, "Kernel process exists");
    TEST_ASSERT_EQUAL(PROCESS_TYPE_KERNEL, kernel_process->type, "Kernel process type");
    TEST_ASSERT_EQUAL(PROCESS_STATE_RUNNING, kernel_process->state, "Kernel process state");
}

/**
 * Test process creation
 */
static void test_process_create(void) {
    boot_log("Testing process creation...");
    
    process_create_params_t params = {
        .name = "test_process",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0x500000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *process = NULL;
    status_t result = process_create(&params, &process);
    
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process creation");
    TEST_ASSERT_NOT_NULL(process, "Process pointer not null");
    TEST_ASSERT_EQUAL(PROCESS_STATE_READY, process->state, "Process in ready state");
    TEST_ASSERT_EQUAL(PROCESS_TYPE_USER, process->type, "Process type");
    TEST_ASSERT_EQUAL(PRIORITY_NORMAL, process->priority, "Process priority");
    TEST_ASSERT_EQUAL(KERNEL_PROCESS_ID, process->parent_pid, "Process parent");
    
    /* Verify process name */
    TEST_ASSERT_EQUAL(0, strcmp(process->name, "test_process"), "Process name");
    
    /* Verify process is valid */
    TEST_ASSERT(process_is_valid(process->pid), "Process is valid");
}

/**
 * Test process destruction
 */
static void test_process_destroy(void) {
    boot_log("Testing process destruction...");
    
    /* Create a test process */
    process_create_params_t params = {
        .name = "test_destroy",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0x600000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *process = NULL;
    status_t result = process_create(&params, &process);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process creation for destroy test");
    
    uint32_t pid = process->pid;
    
    /* Destroy the process */
    result = process_destroy(pid);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process destruction");
    
    /* Verify process is no longer valid */
    TEST_ASSERT(!process_is_valid(pid), "Process no longer valid");
    
    /* Verify we can't get the process */
    process_t *destroyed_process = process_get_by_pid(pid);
    TEST_ASSERT_NULL(destroyed_process, "Destroyed process is null");
}

/**
 * Test process state management
 */
static void test_process_states(void) {
    boot_log("Testing process state management...");
    
    /* Create a test process */
    process_create_params_t params = {
        .name = "test_states",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0x700000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *process = NULL;
    status_t result = process_create(&params, &process);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process creation for state test");
    
    uint32_t pid = process->pid;
    
    /* Test initial state */
    TEST_ASSERT_EQUAL(PROCESS_STATE_READY, process_get_state(pid), "Initial state is ready");
    
    /* Test blocking process */
    result = process_block(pid);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process blocking");
    TEST_ASSERT_EQUAL(PROCESS_STATE_BLOCKED, process_get_state(pid), "Process blocked state");
    
    /* Test unblocking process */
    result = process_unblock(pid);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process unblocking");
    TEST_ASSERT_EQUAL(PROCESS_STATE_READY, process_get_state(pid), "Process ready after unblock");
    
    /* Test setting state directly */
    result = process_set_state(pid, PROCESS_STATE_RUNNING);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Direct state setting");
    TEST_ASSERT_EQUAL(PROCESS_STATE_RUNNING, process_get_state(pid), "Process running state");
    
    /* Clean up */
    process_destroy(pid);
}

/**
 * Test process relationships
 */
static void test_process_relationships(void) {
    boot_log("Testing process relationships...");
    
    /* Create parent process */
    process_create_params_t parent_params = {
        .name = "test_parent",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0x800000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *parent = NULL;
    status_t result = process_create(&parent_params, &parent);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Parent process creation");
    
    /* Create child process */
    process_create_params_t child_params = {
        .name = "test_child",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = parent->pid,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0x900000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *child = NULL;
    result = process_create(&child_params, &child);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Child process creation");
    
    /* Test parent-child relationship */
    TEST_ASSERT_EQUAL(parent->pid, process_get_parent(child->pid), "Child parent relationship");
    TEST_ASSERT_EQUAL(1, parent->child_count, "Parent child count");
    TEST_ASSERT_EQUAL(child->pid, parent->children[0], "Parent child list");
    
    /* Clean up */
    process_destroy(child->pid);
    process_destroy(parent->pid);
}

/**
 * Test process statistics
 */
static void test_process_statistics(void) {
    boot_log("Testing process statistics...");
    
    /* Get initial statistics */
    process_stats_t initial_stats;
    status_t result = process_get_stats(&initial_stats);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Get initial statistics");
    
    /* Create a process */
    process_create_params_t params = {
        .name = "test_stats",
        .type = PROCESS_TYPE_USER,
        .priority = PRIORITY_NORMAL,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0xA00000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = false
    };
    
    process_t *process = NULL;
    result = process_create(&params, &process);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Process creation for stats test");
    
    /* Get updated statistics */
    process_stats_t updated_stats;
    result = process_get_stats(&updated_stats);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Get updated statistics");
    
    /* Verify statistics changed */
    TEST_ASSERT(updated_stats.total_processes > initial_stats.total_processes, 
                "Total processes increased");
    TEST_ASSERT(updated_stats.active_processes > initial_stats.active_processes, 
                "Active processes increased");
    
    /* Clean up */
    process_destroy(process->pid);
}

/**
 * Test quantum-aware processes
 */
static void test_quantum_processes(void) {
    boot_log("Testing quantum-aware processes...");
    
    /* Create quantum-aware process */
    process_create_params_t params = {
        .name = "test_quantum",
        .type = PROCESS_TYPE_QUANTUM,
        .priority = PRIORITY_HIGH,
        .parent_pid = KERNEL_PROCESS_ID,
        .entry_point = (void*)dummy_process_entry,
        .stack_address = (void*)0xB00000,
        .stack_size = PROCESS_STACK_SIZE,
        .is_quantum_aware = true
    };
    
    process_t *process = NULL;
    status_t result = process_create(&params, &process);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Quantum process creation");
    
    /* Test quantum properties */
    TEST_ASSERT_EQUAL(PROCESS_TYPE_QUANTUM, process->type, "Quantum process type");
    TEST_ASSERT(process_is_quantum_aware(process->pid), "Process is quantum aware");
    TEST_ASSERT_EQUAL(0, process->quantum.qubit_allocation, "Initial qubit allocation");
    
    /* Test qubit allocation */
    result = process_allocate_qubits(process->pid, 8);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Qubit allocation");
    TEST_ASSERT_EQUAL(8, process->quantum.qubit_allocation, "Updated qubit allocation");
    
    /* Test qubit deallocation */
    result = process_deallocate_qubits(process->pid, 4);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, result, "Qubit deallocation");
    TEST_ASSERT_EQUAL(4, process->quantum.qubit_allocation, "Remaining qubit allocation");
    
    /* Clean up */
    process_destroy(process->pid);
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

/**
 * Run all process management tests
 */
void run_process_tests(void) {
    boot_log("=== Starting Process Management Tests ===");
    
    /* Reset test counters */
    test_count = 0;
    test_passed = 0;
    test_failed = 0;
    
    /* Run tests */
    test_process_init();
    test_process_create();
    test_process_destroy();
    test_process_states();
    test_process_relationships();
    test_process_statistics();
    test_quantum_processes();
    
    /* Print results */
    boot_log("=== Process Management Test Results ===");
    boot_log("Total tests: %d", test_count);
    boot_log("Passed: %d", test_passed);
    boot_log("Failed: %d", test_failed);
    
    if (test_failed == 0) {
        boot_log("All tests PASSED! ✓");
    } else {
        boot_log("Some tests FAILED! ✗");
    }
    
    boot_log("=== Process Management Tests Complete ===");
}
