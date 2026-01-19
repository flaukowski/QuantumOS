/**
 * Simple Process Management Test
 * 
 * This is a basic test to verify the process management system
 * can be compiled and linked correctly. This file can be included
 * in the build to test basic functionality.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/process.h>
#include <kernel/types.h>

/**
 * Simple test function that can be called from kernel_main
 * to verify process management is working.
 */
void test_process_basic(void) {
    // This is a minimal test that should compile without issues
    // In a real environment, this would test actual functionality
    
    // Test that we can get the current process
    process_t *current = process_get_current();
    
    // Test that we can get kernel process
    process_t *kernel = process_get_by_pid(KERNEL_PROCESS_ID);
    
    // Test basic statistics
    process_stats_t stats;
    status_t result = process_get_stats(&stats);
    
    // These calls should not crash and should return valid results
    // In a real test environment, we would verify the results
    (void)current;
    (void)kernel;
    (void)result;
}
