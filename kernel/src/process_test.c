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
#include <kernel/elf.h>
#include <kernel/memory.h>
#include <kernel/syscall.h>

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

/* Assemble a syntactically valid ET_EXEC ELF64 header + one PT_LOAD phdr into
 * `buf` (must be >= 120 bytes) with caller-chosen segment fields, so a boot
 * self-test can drive the loader's rejection paths deterministically. */
#define ELFTEST_BLOB 128
static void elftest_build(uint8_t *buf, uint64_t p_offset, uint64_t p_vaddr, uint64_t p_filesz,
                          uint64_t p_memsz) {
    for (int i = 0; i < ELFTEST_BLOB; i++) {
        buf[i] = 0;
    }
    elf64_ehdr_t *eh = (elf64_ehdr_t *)buf;
    eh->e_ident[0] = 0x7F;
    eh->e_ident[1] = 'E';
    eh->e_ident[2] = 'L';
    eh->e_ident[3] = 'F';
    eh->e_ident[4] = 2;   /* ELFCLASS64 */
    eh->e_type = 2;       /* ET_EXEC */
    eh->e_machine = 0x3E; /* x86-64 */
    eh->e_entry = 0x40000000;
    eh->e_phoff = sizeof(elf64_ehdr_t);
    eh->e_phentsize = sizeof(elf64_phdr_t);
    eh->e_phnum = 1;
    elf64_phdr_t *ph = (elf64_phdr_t *)(buf + sizeof(elf64_ehdr_t));
    ph->p_type = ELF_PT_LOAD;
    ph->p_flags = ELF_PF_R | ELF_PF_W;
    ph->p_offset = p_offset;
    ph->p_vaddr = p_vaddr;
    ph->p_filesz = p_filesz;
    ph->p_memsz = p_memsz;
    ph->p_align = 0x1000;
}

/* ELF-loader hardening self-test (adversarial bug-hunt of the proc/mem core).
 * Drives the three malformed-spawn rejection paths that must survive WITHOUT
 * panicking or leaking a frame:
 *   A: a non-ELF image (bad magic),
 *   B: a valid ehdr whose PT_LOAD p_offset overflows p_offset+p_filesz (the old
 *      unguarded add wrapped below `size`, passing the check, then read kernel
 *      memory out of bounds into a user page),
 *   C: a valid ehdr whose PT_LOAD p_vaddr sits below USER_VBASE (would map
 *      through the SHARED kernel 2 MB PS pages and corrupt physical memory).
 * Each user_process_spawn_elf must return != STATUS_SUCCESS, and the pmm
 * free-frame count must be IDENTICAL before and after: vmspace_create's
 * PML4+PDPT (and any segment frame) are reclaimed on the error path. Returns 0
 * on success, negative on the first failure.
 *
 * Anti-vacuous: WITHOUT the fixes, A/B/C each leak the 2 address-space frames
 * (free count drops -> returns -4), B additionally spawns successfully off an
 * out-of-bounds read (-2), and C corrupts the kernel half (-3). Any of these
 * fails the boot before "QuantumOS ready", so ci-smoke can never pass vacuously.
 * All three paths return before finalize_user_process, so this is safe to run
 * before the scheduler starts. */
int elf_spawn_selftest(void) {
    uint32_t before = pmm_get_free_frames();
    uint32_t pid = 0;

    static uint8_t blob_a[ELFTEST_BLOB];
    for (int i = 0; i < ELFTEST_BLOB; i++) {
        blob_a[i] = 0; /* bad magic */
    }
    if (user_process_spawn_elf("elftest-bad-magic", blob_a, blob_a + ELFTEST_BLOB, &pid) ==
        STATUS_SUCCESS) {
        return -1;
    }

    static uint8_t blob_b[ELFTEST_BLOB];
    elftest_build(blob_b, 0xFFFFFFFFFFFFF000ULL, 0x40000000ULL, 0x1000, 0x1000);
    if (user_process_spawn_elf("elftest-ofs-overflow", blob_b, blob_b + ELFTEST_BLOB, &pid) ==
        STATUS_SUCCESS) {
        return -2;
    }

    static uint8_t blob_c[ELFTEST_BLOB];
    elftest_build(blob_c, sizeof(elf64_ehdr_t), 0x00100000ULL, 0x1000, 0x1000);
    if (user_process_spawn_elf("elftest-low-vaddr", blob_c, blob_c + ELFTEST_BLOB, &pid) ==
        STATUS_SUCCESS) {
        return -3;
    }

    uint32_t after = pmm_get_free_frames();
    if (after != before) {
        return -4; /* a malformed spawn leaked address-space frames */
    }
    return 0;
}

/* Spawn address-space-leak self-test (cross-cutting bug-hunt, resource-leak
 * class). A spawn that gets a full private address space BUILT (code/stack/arg
 * pages mapped) but then fails at process_create — reachable from ring-3 by
 * filling the 256-slot process table, then every further `run` — must reclaim
 * that address space, or each failed attempt leaks its PML4+PDPT+all mapped
 * frames permanently (a pmm-exhaustion DoS). The fault-injection seam forces the
 * create failure deterministically without filling the table. Returns 0 on pass.
 *
 * Anti-vacuous: WITHOUT finalize_user_process's vmspace_destroy on the
 * create-failure path, the failed spawn below leaks the whole address space, so
 * the free-frame count drops and this returns -2 -> the boot panics before
 * "QuantumOS ready". */
int spawn_leak_selftest(void) {
    static uint8_t blob[16];
    for (int i = 0; i < 16; i++) {
        blob[i] = 0x90; /* NOP-ish; contents irrelevant, spawn fails at create */
    }
    uint32_t before = pmm_get_free_frames();
    uint32_t pid = 0;

    process_test_fail_next_create();
    status_t r = user_process_spawn("spawnleak-test", blob, blob + sizeof(blob), &pid);
    if (r == STATUS_SUCCESS) {
        return -1; /* the injected failure should have prevented creation */
    }

    uint32_t after = pmm_get_free_frames();
    if (after != before) {
        return -2; /* the failed spawn leaked its address space */
    }
    return 0;
}
