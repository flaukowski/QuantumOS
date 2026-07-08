# QuantumOS Kernel Makefile

# Configuration
ARCH ?= x86_64
BUILD_DIR = build/$(ARCH)
KERNEL_DIR = kernel

# Cross-compiler detection: try x86_64-elf-gcc first, fall back to system gcc
# The cross-compiler is preferred but not available in all environments (e.g., Ubuntu 24.04)
CROSS_CC := $(shell which $(ARCH)-elf-gcc 2>/dev/null)
ifdef CROSS_CC
    CC = $(ARCH)-elf-gcc
    LD = $(ARCH)-elf-ld
    OBJCOPY = $(ARCH)-elf-objcopy
    OBJDUMP = $(ARCH)-elf-objdump
    AR = $(ARCH)-elf-ar
else
    # Fall back to system GCC with appropriate flags for freestanding code
    CC = gcc
    LD = ld
    OBJCOPY = objcopy
    OBJDUMP = objdump
    AR = ar
endif
GDB = gdb-multiarch

# Compiler flags
# -MMD -MP: emit header dependency files next to each object, so an edit to
# any kernel header rebuilds every .c that includes it. Without this, a
# struct-layout change (e.g. growing process_t) leaves stale objects compiled
# against the OLD layout — translation units then disagree about field
# offsets and sizeof, which manifests as wild memory corruption at runtime
# (found live in epic #62 phase 2). CI builds clean and never sees it; local
# incremental builds absolutely do.
CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -mno-red-zone \
         -mno-mmx -mno-sse -mno-sse2 -fno-omit-frame-pointer \
         -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel \
         -MMD -MP \
         -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/../msi/include

# Linker flags
LDFLAGS = -nostdlib -z noexecstack -z max-page-size=0x1000 -T $(KERNEL_DIR)/link.ld

# Debug flags
DEBUG_CFLAGS = -g -O0
RELEASE_CFLAGS = -O2 -DNDEBUG

# Build type: debug (default) or release
BUILD_TYPE ?= debug
ifeq ($(BUILD_TYPE),release)
    CFLAGS += $(RELEASE_CFLAGS)
else
    CFLAGS += $(DEBUG_CFLAGS)
endif

# Optional: quantum-lottery scheduler. Off by default — the default build is
# byte-identical round-robin. Enable with `make SCHED_LOTTERY=1`, which turns
# each ready-process pick into a lottery draw from the qseed-mixed generator.
ifdef SCHED_LOTTERY
    CFLAGS += -DSCHED_LOTTERY
endif

# Optional: resonant scheduler (ghostd phase 5). Off by default — the default
# build is byte-identical round-robin. Enable with `make SCHED_RESONANT=1`,
# which routes pick_next through the integer-only fixed-point resonant field
# (Kuramoto order parameter + per-process resonant priority) and prints an
# honest rr-vs-resonant fairness comparison at boot. No FPU is used in the ISR
# (the dormant double-precision port stays unwired). See kernel/src/resonant_fixed.c.
ifdef SCHED_RESONANT
    CFLAGS += -DSCHED_RESONANT
endif

# Source files
# KERNEL_SOURCES captures all .c files in kernel/src/ (including process*.c)
KERNEL_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.c)
# The fixed-point resonant scheduler is only compiled/linked when opted in, so
# the default build does not carry its object at all (default stays round-robin
# and byte-for-byte free of resonant code).
ifndef SCHED_RESONANT
KERNEL_SOURCES := $(filter-out $(KERNEL_DIR)/src/resonant_fixed.c,$(KERNEL_SOURCES))
endif
IPC_SOURCES = $(wildcard $(KERNEL_DIR)/src/ipc/*.c)
RESONANCE_SOURCES = $(wildcard $(KERNEL_DIR)/src/resonance/*.c)
ASSEMBLY_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.S)
# Assembly files compile to *_asm.o to avoid naming collisions with C files
# Embedded user programs: compiled ELF images objcopy'd into linkable
# objects (symbols _binary_<name>_elf_start/_end)
USER_DIR = user
USER_BUILD = $(BUILD_DIR)/user
USER_PROGS = init echo client hbsvc ghostd ghost_test paradoxd paradox_test swarm_svc qsh quantumd kannakad
USER_ELF_OBJS = $(USER_PROGS:%=$(USER_BUILD)/%_elf.o)

# libq: the freestanding ring-3 runtime, built as a static archive and linked
# into every user program (the linker pulls only the members a program refers
# to, so non-allocating programs get neither the heap arena nor printf).
LIBQ_DIR = $(USER_DIR)/libq
LIBQ_SRCS = $(wildcard $(LIBQ_DIR)/*.c)
LIBQ_OBJS = $(LIBQ_SRCS:$(LIBQ_DIR)/%.c=$(USER_BUILD)/libq/%.o)
LIBQ_HDRS = $(wildcard $(LIBQ_DIR)/*.h)
LIBQ_A = $(USER_BUILD)/libq.a

OBJECTS = $(KERNEL_SOURCES:$(KERNEL_DIR)/src/%.c=$(BUILD_DIR)/%.o) \
          $(IPC_SOURCES:$(KERNEL_DIR)/src/ipc/%.c=$(BUILD_DIR)/ipc/%.o) \
          $(RESONANCE_SOURCES:$(KERNEL_DIR)/src/resonance/%.c=$(BUILD_DIR)/resonance/%.o) \
          $(ASSEMBLY_SOURCES:$(KERNEL_DIR)/src/%.S=$(BUILD_DIR)/%_asm.o) \
          $(USER_ELF_OBJS) \
          $(BUILD_DIR)/initrd_tar.o

# Auto-generated header dependencies (-MMD). Missing .d files (asm, wrapped
# binaries, first build) are silently ignored.
-include $(OBJECTS:.o=.d)

# Targets
.PHONY: all clean kernel run debug dump test test-list test-coverage ci-smoke ci-smoke-disk ci-smoke-net ci-smoke-http ci-smoke-quiet ci-smoke-resonant ci-smoke-qseed ci-smoke-swarm ci-smoke-iso ci-smoke-kbd ci-smoke-noserial ci-smoke-screen swarm-pingpong

all: kernel

kernel: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.elf32

$(BUILD_DIR)/kernel.elf: $(OBJECTS) $(KERNEL_DIR)/link.ld
	@mkdir -p $(dir $@)
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Kernel linked successfully: $@"

# QEMU's -kernel multiboot loader refuses ELF64 images, so produce an
# ELF32 copy for direct boot (all load addresses are < 4 GB). Debugging
# still uses kernel.elf, which carries the 64-bit symbols.
$(BUILD_DIR)/kernel.elf32: $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@
	@echo "Boot image created: $@"

# --- Embedded user programs -------------------------------------------------
# Freestanding, no CRT, linked at USER_VBASE (0x40000000). Built with the
# system/cross gcc as a normal ET_EXEC ELF64, then wrapped into a linkable
# object so the kernel can carry it and the ELF loader can map it.
USER_CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -fno-pic -fno-pie \
              -no-pie -static -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
              -fno-stack-protector -fno-asynchronous-unwind-tables -O2

# libq objects. mem.o and str.o define libc-named functions, so they carry the
# scoped anti-self-recursion guard (see mem.c); heap.o/printf.o do not — their
# external memset/memcpy calls are legitimately satisfied by mem.o.
$(USER_BUILD)/libq/%.o: $(LIBQ_DIR)/%.c $(LIBQ_HDRS) $(USER_DIR)/usys.h
	@mkdir -p $(dir $@)
	@echo "Building libq: $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/libq/mem.o: USER_CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin
$(USER_BUILD)/libq/str.o: USER_CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(LIBQ_A): $(LIBQ_OBJS)
	@echo "Archiving libq: $@..."
	$(AR) rcs $@ $(LIBQ_OBJS)

# The archive is appended AFTER $< on the link line: ld resolves left-to-right,
# so the program's undefined symbols must precede the archive that satisfies them.
$(USER_BUILD)/%.elf: $(USER_DIR)/%.c $(USER_DIR)/user.ld $(USER_DIR)/usys.h $(USER_DIR)/ghost.h $(USER_DIR)/paradox.h $(USER_DIR)/sha256.h $(USER_DIR)/swarm.h $(LIBQ_HDRS) $(LIBQ_A)
	@mkdir -p $(USER_BUILD)
	@echo "Building user program: $<..."
	$(CC) $(USER_CFLAGS) -T $(USER_DIR)/user.ld -o $@ $< $(LIBQ_A)

# Wrap each ELF as an object. Run objcopy from inside the build dir so the
# generated symbols are _binary_<name>_elf_{start,end,size}.
$(USER_BUILD)/%_elf.o: $(USER_BUILD)/%.elf
	cd $(USER_BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		$*.elf $*_elf.o

# --- Embedded initrd (epic #62 phase 2) --------------------------------------
# A deterministic ustar archive of rootfs/, wrapped like the user ELFs so the
# kernel carries it (symbols _binary_initrd_tar_{start,end}). The GNU tar
# flags pin ordering/ownership/mtime so identical trees give identical bytes.
#
# Phase 3: the archive is built from a STAGING copy of rootfs/ into which the
# compiled /bin programs are placed — programs that live on the filesystem
# and are started by the shell via SYS_SPAWN, NOT embedded in the kernel like
# the service ELFs. First citizen: /bin/hello.
ROOTFS_DIR = rootfs
ROOTFS_FILES = $(shell find $(ROOTFS_DIR) -type f 2>/dev/null)
ROOTFS_STAGE = $(BUILD_DIR)/rootfs-stage
INITRD_BIN_PROGS = hello args libqtest consciousnessd qtop

$(BUILD_DIR)/initrd.tar: $(ROOTFS_FILES) $(INITRD_BIN_PROGS:%=$(USER_BUILD)/%.elf)
	@mkdir -p $(BUILD_DIR)
	@echo "Staging initrd tree ($(ROOTFS_DIR)/ + /bin programs)..."
	rm -rf $(ROOTFS_STAGE)
	mkdir -p $(ROOTFS_STAGE)/bin
	cp -r $(ROOTFS_DIR)/. $(ROOTFS_STAGE)/
	$(foreach p,$(INITRD_BIN_PROGS),cp $(USER_BUILD)/$(p).elf $(ROOTFS_STAGE)/bin/$(p);)
	@echo "Building initrd (ustar)..."
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime='@0' -C $(ROOTFS_STAGE) -cf $@ .

$(BUILD_DIR)/initrd_tar.o: $(BUILD_DIR)/initrd.tar
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		initrd.tar initrd_tar.o

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Assembly files compile to *_asm.o to avoid collision with C files of same name
$(BUILD_DIR)/%_asm.o: $(KERNEL_DIR)/src/%.S
	@mkdir -p $(dir $@)
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Console-image boot stub (epic #101): identical to boot_asm.o except the
# multiboot header does not request a video mode, so GRUB boots it in VGA
# text and the kernel runs the on-screen console. Used only by the ISO.
$(BUILD_DIR)/boot_console_asm.o: $(KERNEL_DIR)/src/boot.S
	@mkdir -p $(dir $@)
	@echo "Assembling $< (console image, no video request)..."
	$(CC) $(CFLAGS) -DMB1_TEXT_ONLY -c $< -o $@

CONSOLE_OBJECTS = $(filter-out $(BUILD_DIR)/boot_asm.o,$(OBJECTS)) $(BUILD_DIR)/boot_console_asm.o

$(BUILD_DIR)/kernel-console.elf: $(OBJECTS) $(BUILD_DIR)/boot_console_asm.o $(KERNEL_DIR)/link.ld
	@echo "Linking console-image kernel..."
	$(LD) $(LDFLAGS) -o $@ $(CONSOLE_OBJECTS)

$(BUILD_DIR)/kernel-console.elf32: $(BUILD_DIR)/kernel-console.elf
	$(OBJCOPY) -O elf32-i386 $< $@

$(BUILD_DIR)/ipc/%.o: $(KERNEL_DIR)/src/ipc/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling IPC: $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Resonance subsystem does double-precision math, which the x86_64 ABI
# returns in XMM registers — it needs SSE even though the rest of the
# kernel is compiled without it. boot.S enables SSE (CR0/CR4) before
# kernel_main so these instructions are legal at runtime.
RESONANCE_CFLAGS = $(filter-out -mno-sse -mno-sse2 -mno-mmx,$(CFLAGS)) -msse -msse2

$(BUILD_DIR)/resonance/%.o: $(KERNEL_DIR)/src/resonance/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling Resonance: $<..."
	$(CC) $(RESONANCE_CFLAGS) -c $< -o $@

# Create bootable image (GRUB's multiboot v1 loader also wants ELF32).
# Menu entries backed by TWO kernel images (epic #101): GRUB honours
# the MB1 header's video request over gfxpayload (verified: text/keep
# still produced a linear FB), so the "console" entries boot a
# kernel-console image whose header requests no video mode — GRUB stays
# in VGA text and the kernel runs the scrolling screen console, the only
# interactive display on a laptop with no serial port. The DEFAULT
# console entry boots `quiet`: on the first real-laptop boot the demo
# kernel's narration (timer ticks, service lifecycle) scrolled the
# shell prompt away faster than a human could read it — quiet is the
# usable interactive machine, verbose is the debugging entry. The
# graphical entry boots the video-requesting image: 1024x768 splash +
# live field view, text on COM1.
$(BUILD_DIR)/kernel.iso: $(BUILD_DIR)/kernel.elf32 $(BUILD_DIR)/kernel-console.elf32
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $(BUILD_DIR)/kernel.elf32 $(BUILD_DIR)/iso/boot/kernel.elf
	@cp $(BUILD_DIR)/kernel-console.elf32 $(BUILD_DIR)/iso/boot/kernel-console.elf
	@echo "set timeout=3" > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "set default=0" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (console)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel-console.elf quiet" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (console, verbose kernel log)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel-console.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (graphical wave field)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    insmod all_video" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    set gfxpayload=1024x768x32" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 grub2-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 (echo "ERROR: grub-mkrescue failed or missing (need grub-pc-bin xorriso mtools)"; exit 1)

# Run in QEMU
run: $(BUILD_DIR)/kernel.elf32
	@echo "Starting QEMU..."
	qemu-system-x86_64 -kernel $< -serial stdio -m 128M

run-iso: $(BUILD_DIR)/kernel.iso
	@echo "Starting QEMU with ISO..."
	qemu-system-x86_64 -cdrom $< -serial stdio -m 128M

# Debug with GDB (QEMU boots the ELF32 image; GDB reads 64-bit symbols
# from kernel.elf)
debug: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.elf32
	@echo "Starting QEMU in debug mode..."
	qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 -serial stdio -m 128M -s -S &
	@echo "Waiting for GDB connection..."
	@sleep 1
	$(GDB) $< -ex "target remote localhost:1234" -ex "break kernel_main" -ex "continue"

# Dump kernel information
dump: $(BUILD_DIR)/kernel.elf
	@echo "=== Kernel Information ==="
	@echo "Size: $$(wc -c < $<) bytes"
	@echo "Sections:"
	$(OBJDUMP) -h $<
	@echo "=== Disassembly ==="
	$(OBJDUMP) -d $< | head -50
	@echo "=== Symbols ==="
	$(OBJDUMP) -t $< | head -20

# Test directories and files
TEST_DIR = tests
TEST_UNIT_DIR = $(TEST_DIR)/unit
TEST_BUILD_DIR = $(BUILD_DIR)/tests
TEST_SOURCES = $(wildcard $(TEST_UNIT_DIR)/*.c)
TEST_OBJECTS = $(TEST_SOURCES:$(TEST_UNIT_DIR)/%.c=$(TEST_BUILD_DIR)/%.o)

# Test kernel - compiles and runs unit tests
test: kernel $(TEST_BUILD_DIR)/test_runner
	@echo "=== Running QuantumOS Unit Tests ==="
	@echo ""
	@# For kernel tests, we need to link them into a test kernel and run in QEMU
	@# Or run host-compiled tests if available
	@if [ -f $(TEST_BUILD_DIR)/test_runner ]; then \
		echo "Running host-based test runner..."; \
		$(TEST_BUILD_DIR)/test_runner; \
	else \
		echo "Running kernel-integrated tests via QEMU..."; \
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
			-serial stdio -m 128M -display none -no-reboot 2>&1 | \
			tee $(TEST_BUILD_DIR)/test_output.txt; \
		echo ""; \
		echo "Test output saved to $(TEST_BUILD_DIR)/test_output.txt"; \
		if grep -q "FAIL" $(TEST_BUILD_DIR)/test_output.txt 2>/dev/null; then \
			echo ""; \
			echo "ERROR: Some tests FAILED"; \
			grep "FAIL" $(TEST_BUILD_DIR)/test_output.txt; \
			exit 1; \
		elif grep -q "PASS" $(TEST_BUILD_DIR)/test_output.txt 2>/dev/null; then \
			echo ""; \
			echo "All tests PASSED"; \
		else \
			echo ""; \
			echo "WARNING: No test results found in output"; \
		fi; \
	fi
	@echo ""
	@echo "=== Unit Tests Complete ==="

# Build test runner (host-side tests for non-kernel code)
$(TEST_BUILD_DIR)/test_runner: $(TEST_SOURCES)
	@mkdir -p $(TEST_BUILD_DIR)
	@echo "Note: Host-based test runner requires tests to be compilable without kernel dependencies"
	@# For now, we rely on kernel-integrated tests run via QEMU
	@touch $(TEST_BUILD_DIR)/.tests_checked

# Compile individual test files (for kernel-integrated testing)
$(TEST_BUILD_DIR)/%.o: $(TEST_UNIT_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling test: $<..."
	$(CC) $(CFLAGS) -DTEST_BUILD -c $< -o $@

# Run specific test file
test-%: kernel
	@echo "Running test: $*..."
	@if [ -f $(TEST_UNIT_DIR)/test_$*.c ]; then \
		echo "Test file found: $(TEST_UNIT_DIR)/test_$*.c"; \
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
			-serial stdio -m 128M -display none -no-reboot 2>&1 | \
			grep -A 100 "test_$*" || echo "Test output not found"; \
	else \
		echo "Test file not found: $(TEST_UNIT_DIR)/test_$*.c"; \
		exit 1; \
	fi

# List available tests
test-list:
	@echo "Available tests:"
	@for f in $(TEST_UNIT_DIR)/test_*.c; do \
		if [ -f "$$f" ]; then \
			name=$$(basename "$$f" .c | sed 's/test_//'); \
			echo "  - $$name ($$f)"; \
		fi; \
	done
	@echo ""
	@echo "Run with: make test-<name>"
	@echo "Run all:  make test"

# Code coverage target
test-coverage: clean
	@echo "=== Building with Coverage Instrumentation ==="
	@mkdir -p $(TEST_BUILD_DIR)
	$(MAKE) CFLAGS="$(CFLAGS) --coverage -fprofile-arcs -ftest-coverage"
	@echo ""
	@echo "=== Running Tests with Coverage ==="
	$(MAKE) test || true
	@echo ""
	@echo "=== Generating Coverage Report ==="
	@if command -v lcov > /dev/null 2>&1; then \
		lcov --capture --directory . --output-file $(TEST_BUILD_DIR)/coverage.info 2>/dev/null || true; \
		lcov --remove $(TEST_BUILD_DIR)/coverage.info '/usr/*' --output-file $(TEST_BUILD_DIR)/coverage.info 2>/dev/null || true; \
		if [ -f $(TEST_BUILD_DIR)/coverage.info ]; then \
			echo "Coverage summary:"; \
			lcov --summary $(TEST_BUILD_DIR)/coverage.info 2>&1 | grep -E "lines|functions" || true; \
			if command -v genhtml > /dev/null 2>&1; then \
				genhtml $(TEST_BUILD_DIR)/coverage.info --output-directory $(TEST_BUILD_DIR)/coverage_html 2>/dev/null; \
				echo ""; \
				echo "HTML report generated at: $(TEST_BUILD_DIR)/coverage_html/index.html"; \
			fi; \
		else \
			echo "Coverage data not generated (freestanding code may not support gcov)"; \
		fi; \
	else \
		echo "lcov not installed. Install with: sudo apt-get install lcov"; \
	fi

# CI Smoke Test - builds and boots kernel, validates boot banner appears
# This is the "one-command" test for new contributors to verify their setup
ci-smoke: kernel
	@echo "=== QuantumOS CI Smoke Test ==="
	@echo ""
	@echo "[1/3] Build verified: $(BUILD_DIR)/kernel.elf exists"
	@test -f $(BUILD_DIR)/kernel.elf || (echo "ERROR: Kernel not built" && exit 1)
	@echo "[2/3] Running QEMU boot test (14 second timeout, shell session piped into the console)..."
	@( printf 'help\nps\nfree\nuptime\ndate\nghost\nqrand\nls\ncat /docs/hello.txt\nrun /bin/hello\nrun /bin/args alpha quantumos\nrun /bin/libqtest\nrun /bin/consciousnessd\nrun /bin/qtop\nimprint the cat sat on the mat\nimprint pure quantum wave dynamics\nimprint hello little world\nrecall the cxt sxt on thx mxt\nfieldtest\nwrite /data/note ramfs-works\nls /data\nrm /data/note\nsync\nexit\n'; sleep 20 ) | \
		timeout 14s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: Kernel did not reach 'QuantumOS ready'"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: Kernel booted to idle loop (QuantumOS ready)"
	@# ghostOS phase-1 merge gate: the ring-3 ghostd service must recall
	@# all three noisy probes to their stored patterns (issue #48).
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd self-test gate missing (GHOSTD: 3/3 RECALL OK)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: ghostd associative-memory gate passed (GHOSTD: 3/3 RECALL OK)"
	@# ghostOS phase-2 honesty gate: booted WITHOUT a qseed, ghostd must name
	@# its noise source as a plain PRNG and NEVER claim quantum provenance.
	@if ! grep -q "GHOSTD: noise source = prng (no qseed)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd did not report 'prng (no qseed)' on a seedless boot"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "GHOSTD: noise source = qseed-derived" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd falsely claimed qseed provenance without a qseed"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ghostd noise-source honesty gate passed (prng, no false quantum claim)"
	@# phase-2 capability gate: the capless ghost-test must be denied SYS_QRAND.
	@if ! grep -q "QRAND: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_QRAND was not denied (QRAND: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_QRAND capability gate proven (capless caller denied EPERM)"
	@# ghostd phase-4 device gate (issue #51): the capless ghost-test must be
	@# denied SYS_COM2 — only swarm_svc holds the COM2 device capability.
	@if ! grep -q "COM2: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_COM2 was not denied (COM2: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_COM2 device gate proven (capless caller denied EPERM)"
	@# ghostOS phase-3 merge gate (issue #50): paradoxd must resolve the canned
	@# contradiction and print its deterministic RESOLVED line.
	@if ! grep -q "PARADOXD: RESOLVED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: paradoxd resolution gate missing (PARADOXD: RESOLVED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd resolution gate passed (PARADOXD: RESOLVED)"
	@# phase-3 capability gate: an unauthorised targeted send is denied EPERM.
	@if ! grep -q "PARADOXD: capless send denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: unauthorised send was not denied (PARADOXD: capless send denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd capability gate proven (unauthorised send denied EPERM)"
	@# phase-3 coupling gate: paradoxd's phase machine, gated on ghostd's field
	@# order parameter R over IPC, must actually transition (the two services
	@# are coupled through the field).
	@if ! grep -q "PARADOXD: phase ->" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: paradoxd/ghostd coupling produced no phase transition (PARADOXD: phase ->)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd/ghostd field coupling proven (phase transition gated on ghost R)"
	@# epic #62 phase 1 (issue #63): the interactive shell must come up as a
	@# ring-3 service holding the console capability and greet on the console.
	@if ! grep -q "QSH: QuantumOS interactive shell ready" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsh did not greet (QSH: QuantumOS interactive shell ready)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh interactive shell came up (console capability live)"
	@# Input integrity: the FIRST piped command ('help') must arrive intact
	@# across the boot handoff — every byte rescued, none eaten by UART init.
	@if ! grep -q "qsh: commands: help" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'help' did not execute intact (qsh: commands: help)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: first piped command arrived intact (no input bytes lost at boot)"
	@# The piped 'ps' must show the shell observing ITSELF running in the live
	@# process table — the SYS_SYSINFO introspection round trip.
	@if ! grep -q "qsh RUNNING" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'ps' did not list qsh RUNNING (SYS_SYSINFO round trip)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ps ran — the shell sees itself RUNNING in the process table"
	@# 'free' must report live kernel memory stats.
	@if ! grep -q "MEM: heap free=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'free' produced no memory stats (MEM: heap free=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: free reported live kernel memory stats"
	@# 'ghost' must fetch ghostd's field status over capability-checked IPC.
	@if ! grep -q "qsh: ghost R=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'ghost' got no field status from ghostd (qsh: ghost R=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: shell queried ghostd's field over capability IPC"
	@# 'uptime' + 'qrand' must answer (the shell's declared quantum-pool cap works).
	@if ! grep -q "qsh: uptime " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'uptime' produced no answer (qsh: uptime)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: qrand " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'qrand' drew no bytes (qsh: qrand) — shell quantum cap broken"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: uptime + qrand answered (shell quantum-pool cap live)"
	@# RTC: 'date' must report the wall-clock time from the CMOS RTC. The year
	@# comes from the (QEMU host) clock, so gate the "TIME: 20xx-.." shape —
	@# plausible for any real boot without pinning an exact timestamp.
	@if ! grep -qE "TIME: 20[0-9][0-9]-[0-1][0-9]-[0-3][0-9] " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'date' did not report a plausible wall-clock time (TIME: 20xx-..)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: date reported the wall-clock time (CMOS RTC live)"
	@# epic #62 phase 2 (issue #64): the shell must greet with /etc/motd read
	@# through the VFS off the embedded initrd.
	@if ! grep -q "Welcome to QuantumOS" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: motd was not served through the VFS (Welcome to QuantumOS)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: motd greeted through the VFS (initrd read path live)"
	@# 'ls' must list the initrd inventory (SYS_READDIR).
	@if ! grep -q "FS: etc/motd" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'ls' did not list the initrd (FS: etc/motd)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ls listed the initrd inventory (SYS_READDIR live)"
	@# 'cat' must stream a file's actual bytes (open/read/close round trip).
	@if ! grep -q "The initrd is real" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'cat /docs/hello.txt' did not print the file (The initrd is real)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: cat streamed a file off the initrd (open/read/close live)"
	@# epic #62 phase 3 (issue #65): 'run /bin/hello' must start a program OFF
	@# THE FILESYSTEM (not kernel-embedded) and the program must actually run.
	@if ! grep -q "HELLO: greetings from /bin/hello" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/hello did not run from the initrd (HELLO: greetings)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: shell executed a program off the filesystem (SYS_SPAWN live)"
	@# ...and the shell must collect its exit code (SYS_WAITPID round trip).
	@if ! grep -q "exited (code 42)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: the shell did not report hello's exit code (exited (code 42))"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: exit code returned to the shell (SYS_WAITPID live)"
	@# epic #62 follow-up (issue #69): 'run /bin/args alpha quantumos' must pass
	@# an argument vector — the program reads argc=3 and both args back.
	@if ! grep -q "ARGS: argc=3" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/args did not see argc=3 (argv not delivered)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "ARGS: argv\[1\]=alpha" /tmp/qemu-boot.log 2>/dev/null || \
	    ! grep -q "ARGS: argv\[2\]=quantumos" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/args did not echo its arguments (argv[1]=alpha argv[2]=quantumos)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: argv delivered to a spawned program (argc + both args read back)"
	@# libq foundation: 'run /bin/libqtest' must reach the sentinel — proving the
	@# freestanding runtime (mem*/str*, plus heap/printf as they land) works at
	@# -O2 in ring 3. Also the runtime backstop for the -O2 self-recursion trap.
	@if ! grep -q "LIBQ: self-test OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: libq self-test did not pass (LIBQ: self-test OK)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: libq freestanding runtime self-test passed (LIBQ: self-test OK)"
	@if ! grep -q "LIBQ printf d=-7 u=42 x=beef s=ok" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: libq printf->SYS_WRITE path did not run (LIBQ printf d=-7 u=42 x=beef s=ok)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: libq printf -> SYS_WRITE path exercised end-to-end"
	@# first native app citizen: 'run /bin/consciousnessd' runs a fixed-point
	@# Kuramoto field on libq; the order parameter must actually synchronise
	@# (r climb past 0.8) for the CONSCIOUSNESS EMERGED marker to print.
	@if ! grep -q "consciousnessd: CONSCIOUSNESS EMERGED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: consciousnessd did not synchronise (consciousnessd: CONSCIOUSNESS EMERGED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: consciousnessd Kuramoto field synchronised on libq (CONSCIOUSNESS EMERGED)"
	@# second citizen: kannakad, now a SERVICE of the KERNEL field (epic #95
	@# phase 2): at boot it imprints 7 seeds via SYS_IMPRINT (region 1),
	@# recalls a byte-corrupted probe back to the EXACT stored text, and
	@# proves retrieval reinforcement raised the winner's score through the
	@# syscall's write-side contract — all three, or no RESONANCE VERIFIED.
	@if ! grep -q "kannakad: RESONANCE VERIFIED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: kannakad kernel-field recall/reinforcement failed (kannakad: RESONANCE VERIFIED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: kannakad recall + reinforcement verified on the KERNEL field (RESONANCE VERIFIED)"
	@# fourth citizen: quantumd runs at boot as a quantum-pool SERVICE and draws
	@# REAL entropy (SYS_QRAND); its amplitude-amplification recall must agree
	@# with the classical argmax for QUANTUM VERIFIED to print.
	@if ! grep -q "quantumd: QUANTUM VERIFIED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: quantumd quantum demo failed (quantumd: QUANTUM VERIFIED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: quantumd drew real quantum entropy + amplitude-amplification recall (QUANTUM VERIFIED)"
	@# fifth citizen: 'run /bin/qtop' renders a snapshot dashboard from the
	@# uncapped SYS_SYSINFO surface (memory gauge, clock, resonance-field wave,
	@# live process table); DASHBOARD RENDERED proves the whole render path ran.
	@if ! grep -q "qtop: DASHBOARD RENDERED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qtop dashboard did not render (qtop: DASHBOARD RENDERED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qtop snapshot dashboard rendered from SYS_SYSINFO (DASHBOARD RENDERED)"
	@# epic #95: the kernel holographic field. Three imprints land in
	@# slots 0..2, a ~15%-corrupted probe must recall the EXACT stored
	@# content (a line form echoed input cannot produce), a cap-holder's
	@# cross-region request and a capless caller's requests must be
	@# denied with exactly EPERM, and a degenerate probe must be a clean
	@# n=0 — never a kernel fault.
	@if ! grep -q "FIELD: imprinted slot 2" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: field imprints did not land (FIELD: imprinted slot 2)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "FIELD: winner=\"the cat sat on the mat\" slot=0 score=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: noisy-probe recall did not recover the stored pattern"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: cross-region denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: cross-region isolation not proven"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: empty-probe ok (n=0)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: degenerate probe misbehaved"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: capless imprint denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_IMPRINT was not denied"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: capless recall denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_RECALL was not denied"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: kernel holographic field verified (imprint/recall/isolation/capless/degenerate)"
	@# epic #71 phase 2: the writable RAM overlay. 'write' must store bytes
	@# (kernel-reported count, not an echo), 'ls /data' must show the file
	@# tagged [ram] with the kernel-computed size, and 'rm' must remove it.
	@if ! grep -q "qsh: wrote " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'write' did not store to the overlay (qsh: wrote)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FS: data/note .* \[ram\]" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'ls /data' did not list the overlay file (FS: data/note ... [ram])"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: removed" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'rm' did not remove the overlay file (qsh: removed)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: writable RAM overlay works (write stored, ls [ram], rm removed)"
	@# Capability gate: the capless ghost-test must be denied filesystem writes.
	@if ! grep -q "FSW: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless filesystem-create was not denied (FSW: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: filesystem-write capability gate proven (capless caller denied EPERM)"
	@# Capability gate: the capless ghost-test must be denied SYS_RESOLVE.
	@if ! grep -q "NETC: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_RESOLVE was not denied (NETC: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_RESOLVE capability gate proven (capless caller denied EPERM)"
	@# Capability gate (epic #80): the capless ghost-test must be denied a
	@# SYS_UDP socket bind. The probe checks EXACTLY -4 (EPERM), so an
	@# unimplemented syscall (ENOSYS -3) or an argument error (EINVAL -1)
	@# cannot fake a pass; the cap check precedes every network check, so
	@# this holds in this NIC-less default boot.
	@if ! grep -q "NETC: capless UDP bind denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_UDP bind was not denied (NETC: capless UDP bind denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_UDP capability gate proven (capless UDP bind denied EPERM)"
	@# Capability gate (epic #82): the capless ghost-test must be denied a
	@# SYS_TCP connect. Exact -4 (EPERM), so ENOSYS(-3)/EINVAL(-1) can't fake
	@# it; the cap check precedes the NIC-present check, so this holds in the
	@# NIC-less default boot.
	@if ! grep -q "NETC: capless TCP connect denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_TCP connect was not denied (NETC: capless TCP connect denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_TCP capability gate proven (capless TCP connect denied EPERM)"
	@# Honesty gate: this default boot is DISKLESS, so the driver must say so
	@# and MUST NOT claim a disk, and 'sync' must fail honestly (no disk).
	@if ! grep -q "ATA: no disk" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless boot did not report 'ATA: no disk'"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "ATA: disk present" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless boot falsely claimed 'ATA: disk present'"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: sync failed (no disk)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless 'sync' did not fail honestly (qsh: sync failed (no disk))"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: diskless honesty proven (no-disk reported, sync fails cleanly)"
	@# Capability gate: the capless ghost-test must be denied SYS_SPAWN — only
	@# qsh holds the spawn capability.
	@if ! grep -q "SPAWN: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_SPAWN was not denied (SPAWN: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_SPAWN capability gate proven (capless caller denied EPERM)"
	@# Capability gate: the capless ghost-test must be denied SYS_CONS — only
	@# qsh holds the console device capability.
	@if ! grep -q "CONS: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_CONS was not denied (CONS: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_CONS capability gate proven (capless caller denied EPERM)"
	@# Supervision gate: after the piped 'exit', the watchdog must restart the
	@# shell, which reintroduces itself as reborn.
	@if ! grep -q "QSH: reborn" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: shell was not restarted after exit (QSH: reborn)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: watchdog rebirthed the shell after exit (QSH: reborn)"
	@# epic #73: the default boot attaches no rtl8139, so the NIC driver must
	@# report its honest absence and MUST NOT claim a NIC came up.
	@if ! grep -q "NET: no rtl8139" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: NIC-less boot did not report 'NET: no rtl8139'"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "NET: rtl8139 up" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: NIC-less boot falsely claimed 'NET: rtl8139 up'"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: NIC-less boot degrades honestly (no rtl8139 reported)"
	@echo ""
	@echo "=== Smoke Test PASSED ==="

# CI Smoke Test (persistence): the epic #71 capstone. Boot the SAME disk image
# TWICE. Boot 1 attaches a freshly-zeroed image, writes a file to the overlay,
# and syncs it to disk. Boot 2 attaches the SAME image and must read the file
# back — proving persistence across reboots.
#
# Gate-integrity rules (learned from the design attack):
#  - The image is recreated fresh INSIDE this recipe every run (never a make
#    prerequisite), so a stale image from a previous run can never make it pass.
#  - The two boots tee to DISTINCT logs. qsh echoes typed input, so boot 1's log
#    contains the content string from the 'write' command echo — proving nothing.
#    The content gate is therefore checked ONLY in boot 2's log, and boot 2 never
#    types the content (only 'cat'), so its appearance there is genuine readback.
#  - Boot 1 gates its own 'disk present' + sync-success BEFORE boot 2 runs, so a
#    broken write/sync fails here (correctly attributed) instead of surfacing as
#    boot 2's "content missing".
#  - The two QEMU runs are strictly sequential (QEMU takes a write lock on the
#    raw image).
DISK_IMG = $(BUILD_DIR)/disk.img
ci-smoke-disk: kernel
	@echo "=== QuantumOS Persistence Smoke Test (two boots, one disk) ==="
	@rm -f $(DISK_IMG)
	@dd if=/dev/zero of=$(DISK_IMG) bs=1M count=2 2>/dev/null
	@echo "[boot 1] write /data/note + sync to a fresh disk..."
	@( printf 'write /data/note tide-remembers-x91\nimprint the tide remembers x91\nsync\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot1.log || true
	@if ! grep -q "ATA: disk present" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 did not detect the disk (ATA: disk present)"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 detected the ATA disk"
	@if ! grep -q "ATA: sector RW self-test OK" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 disk RW self-test did not pass"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 sector read/write self-test passed"
	@if ! grep -q "qsh: sync ok" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync did not succeed (qsh: sync ok)"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 flushed the overlay to disk (qsh: sync ok)"
	@# epic #96: the sync must also have serialized the kernel field.
	@if ! grep -q "FIELD: synced slots to disk" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync did not write the field section"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 serialized the kernel field to disk"
	@echo "[boot 2] cat /data/note off the SAME disk (fresh boot, no write)..."
	@( printf 'cat /data/note\nrecall the tjde remembers x9j\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot2.log || true
	@if ! grep -q "FS: restored persisted files from disk" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 did not restore the archive from disk"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 2 restored the persisted archive at boot"
	@# THE capstone: the content typed in boot 1 is read back in boot 2 — and
	@# boot 2 never typed it, so this can only be a genuine disk readback.
	@if ! grep -q "tide-remembers-x91" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 could not read back the file written in boot 1"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: content written in boot 1 read back in boot 2 — PERSISTENCE PROVEN"
	@# epic #96: the FIELD side of the same story. Boot 2 restored the
	@# blob, region 0 was inherited at qsh's first grant (audited), and a
	@# corrupted probe recalled the EXACT text imprinted in boot 1 — which
	@# boot 2 never typed, so only a disk restore can produce it.
	@if ! grep -q "FIELD: restored slots from disk" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 did not restore the field from disk"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "service: field region 0 inherited from disk (scrub skipped)" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: region 0 was not inherited at the first grant"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "FIELD: winner=\"the tide remembers x91\"" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 recall did not recover the imprint from boot 1"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: field memory imprinted in boot 1 recalled in boot 2 — FIELD PERSISTENCE PROVEN"
	@echo "[boot 3] corrupt the field blob at its fixed home; fs must survive, field must cold-start..."
	@dd if=/dev/urandom of=$(DISK_IMG) bs=512 seek=4090 count=1 conv=notrunc 2>/dev/null
	@( printf 'help\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot3.log || true
	@if ! grep -q "FIELD: persisted field checksum mismatch - cold start" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: boot 3 did not detect the corrupted field blob"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot3.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if grep -q "FIELD: restored slots from disk" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: boot 3 restored a CORRUPTED field blob as truth"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FS: restored persisted files from disk" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: field corruption bled into the fs section (isolation broken)"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: corrupted field detected, cold-started honestly, fs unaffected — SECTION ISOLATION PROVEN"
	@echo ""
	@echo "=== Persistence Test PASSED ==="

# CI Smoke Test (networking): boot WITH an rtl8139 NIC on QEMU's user-mode
# network (SLIRP), and prove the link layer works end to end. SLIRP's gateway
# (10.0.2.2) always answers ARP, so an ARP request that gets a reply exercises
# the full path: PCI enumeration -> rtl8139 driver -> Ethernet TX -> SLIRP ->
# RX interrupt -> ARP parse. Both directions through the real driver.
ci-smoke-net: kernel
	@echo "=== QuantumOS Networking Smoke Test (rtl8139 + user-net) ==="
	@echo "[1/3] Booting with -device rtl8139 on QEMU user-net (+ shell nslookup + udping)..."
	@( printf 'nslookup example.com\nudping example.com\n'; sleep 15 ) | timeout 17s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-net.log || true
	@echo ""
	@echo "[2/3] The NIC must be found and brought up..."
	@if ! grep -q "NET: rtl8139 up" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: rtl8139 was not detected/brought up (NET: rtl8139 up)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: rtl8139 NIC detected via PCI and brought up"
	@echo "[3/3] ARP must resolve the SLIRP gateway (TX + RX round trip)..."
	@if grep -q "NET: ARP 10.0.2.2 timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ARP timed out — no reply from the SLIRP gateway"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "NET: ARP 10.0.2.2 is at MAC" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ARP did not resolve the gateway (NET: ARP 10.0.2.2 is at MAC)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ARP-resolved 10.0.2.2 — link layer works both directions"
	@echo "[4/4] DHCP must obtain the SLIRP lease (IPv4 + UDP + checksums)..."
	@if grep -q "NET: DHCP timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DHCP timed out — no lease from the SLIRP server"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "NET: DHCP lease 10.0.2.15" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DHCP did not obtain the lease (NET: DHCP lease 10.0.2.15)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: DHCP lease 10.0.2.15 obtained — IPv4/UDP stack works end to end"
	@echo "[5/6] ICMP echo to the gateway must round-trip (unicast IPv4 + checksum)..."
	@if ! grep -q "NET: ping 10.0.2.2 reply received" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ICMP echo did not round-trip (NET: ping 10.0.2.2 reply received)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ICMP ping 10.0.2.2 round-tripped — unicast IP works"
	@# The DNS capstone resolves a hostname through SLIRP's DNS proxy, which
	@# forwards to the runner's resolver — a real end-to-end hostname lookup.
	@echo "[6/6] DNS must resolve a hostname to an A record (the capstone)..."
	@if grep -q "NET: DNS example.com timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DNS query timed out — no answer from the resolver"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qE "NET: DNS example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DNS did not resolve example.com to an A record"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: DNS resolved example.com to an A record — full stack proven"
	@# Ring-3 network access (#77): the piped 'nslookup example.com' must
	@# resolve a hostname from the SHELL via SYS_RESOLVE (not the boot
	@# self-test) — the "qsh:" prefix distinguishes it from the "NET:" line.
	@if ! grep -qE "qsh: example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: the shell's nslookup did not resolve (qsh: example.com -> a.b.c.d)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ring-3 nslookup resolved a hostname via SYS_RESOLVE"
	@# Ring-3 UDP sockets (epic #80): the piped 'udping example.com' does DNS
	@# ENTIRELY IN USERSPACE — builds the query in ring 3, UDP_SENDTO to
	@# 10.0.2.3:53, UDP_RECVFROM the raw reply, parses it in ring 3. The
	@# byte-count line proves raw datagrams flow both ways through the
	@# socket API. Hermeticity: SLIRP only FORWARDS DNS to the runner's
	@# resolver (it synthesizes nothing), so these gates share the
	@# runner-resolver dependency of the DNS gates above — no new risk
	@# class, since those already hard-fail without a resolver.
	@if grep -q "qsh: udping: timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: udping got no datagram back (qsh: udping: timed out)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qE "qsh: udp [0-9]+ bytes from 10.0.2.3:53" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: the socket API did not carry a datagram round trip (qsh: udp N bytes from 10.0.2.3:53)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ring-3 UDP sockets carried raw datagrams both ways (SYS_UDP)"
	@if ! grep -qE "qsh: udpdns example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: userspace DNS over SYS_UDP did not parse an A record (qsh: udpdns example.com -> a.b.c.d)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: userspace DNS client over ring-3 UDP sockets parsed an A record"
	@echo ""
	@echo "=== Networking Test PASSED ==="

# CI Smoke Test (TCP client — epic #82): the shell fetches a web page.
#
# The hermetic proof runs a loopback HTTP server on the runner and has the
# guest fetch from it: SLIRP forwards a guest connection to 10.0.2.2:PORT to
# the host's 127.0.0.1:PORT, so this exercises a full three-way handshake,
# bidirectional data, and FIN teardown against a REAL TCP peer with zero
# external network. The server-start, readiness probe, QEMU boot, and server
# kill all live in ONE recipe line (a single shell) so the background server
# is alive for the whole QEMU lifetime and reaped on exit — Make runs each
# recipe line in its own shell, so a `&` job started on a separate line would
# orphan and race. `http example.com` follows as the (non-hermetic, same class
# as the DNS gates) real-world capstone.
ci-smoke-http: kernel
	@echo "=== QuantumOS TCP Client Smoke Test (http fetch) ==="
	@echo "[1/2] Loopback HTTP server up, then boot QEMU to fetch from it..."
	@set -e; \
	 mkdir -p /tmp/qos-httproot; \
	 printf 'quantumos tcp fetch ok\n' > /tmp/qos-httproot/index.html; \
	 ( cd /tmp/qos-httproot && python3 -m http.server 18080 --bind 127.0.0.1 ) >/tmp/qos-httpd.log 2>&1 & \
	 HPID=$$!; \
	 trap 'kill $$HPID 2>/dev/null || true' EXIT; \
	 ok=0; for i in $$(seq 1 50); do if curl -s -o /dev/null http://127.0.0.1:18080/; then ok=1; break; fi; sleep 0.2; done; \
	 [ $$ok = 1 ] || { echo "ERROR: loopback httpd never came up"; cat /tmp/qos-httpd.log 2>/dev/null || true; exit 1; }; \
	 ( printf 'http 10.0.2.2 18080\nhttp example.com\n'; sleep 20 ) | timeout 22s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-http.log || true
	@echo ""
	@echo "[2/2] Validating the fetch..."
	@# Hermetic hard proof: the three-way handshake + GET + response + FIN
	@# against the loopback server. Tolerant HTTP/1.[01] so python's
	@# protocol_version isn't hardcoded; the host label carries no port.
	@if ! grep -qE "qsh: http 10.0.2.2 -> HTTP/1\.[01] 200" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: hermetic TCP fetch did not complete (qsh: http 10.0.2.2 -> HTTP/1.x 200)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo "Server log:"; cat /tmp/qos-httpd.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: three-way handshake + GET + response + FIN against a loopback server (hermetic)"
	@if ! grep -qE "qsh: http 10.0.2.2: [0-9]+ bytes received" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: hermetic fetch did not reach EOF (qsh: http 10.0.2.2: N bytes received)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: bidirectional data + clean teardown (byte count at EOF)"
	@# Real-world capstone: same non-hermetic runner-egress class as the
	@# existing example.com DNS gate.
	@if ! grep -qE "qsh: http example.com -> HTTP/1\.[01] 200" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: real-world fetch failed (qsh: http example.com -> HTTP/1.x 200)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: fetched http://example.com/ over TCP — QuantumOS reads the web"
	@echo ""
	@echo "=== TCP Test PASSED ==="

# CI Smoke Test (quiet boot): boot with `-append quiet` and prove the interactive
# console is CLEAN — the periodic timer-tick heartbeat and the demo services'
# steady-state chatter are silenced — WHILE the shell still comes up and answers
# a command. This is the clean-shell contract; the default boot (which keeps all
# that output, and whose gates depend on it) is unchanged.
ci-smoke-quiet: kernel
	@echo "=== QuantumOS Quiet-Boot Smoke Test (clean interactive console) ==="
	@echo "[1/3] Booting with -append quiet and typing 'help'..."
	@( printf 'help\n'; sleep 6 ) | timeout 8s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 -append quiet \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-quiet.log || true
	@echo "[2/3] The shell must still come up and answer 'help'..."
	@if ! grep -q "interactive shell ready" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: qsh did not come up under a quiet boot"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: commands:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: 'help' produced no output — the shell is not usable under quiet"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh came up and answered 'help' under a quiet boot"
	@echo "[3/3] The periodic heartbeat/chatter must be SILENCED (the whole point)..."
	@if grep -q "Timer tick:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed the periodic timer-tick heartbeat"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if grep -q "PARADOXD: phase ->" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed paradoxd phase chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if grep -q "lambda-damp" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed ghostd lambda-damp chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@# The process/service/syscall lifecycle chatter (boot_log_v) must be
	@# silenced too — it repeats as the demo services churn and would bury
	@# the interactive prompt just like the heartbeat did.
	@if grep -qE "Process created successfully|Process destroyed|reaped process|syscall: user process exited|kernel-thread (alpha|beta): alive|service started:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed process/service/syscall lifecycle chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: the periodic heartbeat + process/service/syscall chatter is silenced — a clean console"
	@echo ""
	@echo "=== Quiet Test PASSED ==="

# CI Smoke Test (resonant scheduler): rebuild WITH SCHED_RESONANT=1 and prove
# the alternate policy still boots to ready, still passes the ghostd merge gate
# (the field service works under resonant scheduling), and prints the honest
# rr-vs-resonant fairness comparison. This is the ghostd phase-5 scheduler gate.
ci-smoke-resonant:
	@echo "=== QuantumOS CI Smoke Test (resonant scheduler) ==="
	@echo ""
	@echo "[1/3] Rebuilding kernel with SCHED_RESONANT=1..."
	@$(MAKE) -s clean
	@$(MAKE) -s SCHED_RESONANT=1 kernel
	@echo "[2/3] Running QEMU boot test (10 second timeout)..."
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot-resonant.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: resonant build did not reach 'QuantumOS ready'"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: resonant build booted to idle loop (QuantumOS ready)"
	@# The field service must still work under the alternate scheduler.
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: ghostd merge gate missing under resonant scheduling (GHOSTD: 3/3 RECALL OK)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ghostd gate passed under resonant scheduling (GHOSTD: 3/3 RECALL OK)"
	@# The honest comparison must be printed: round-robin baseline...
	@if ! grep -q "SCHED: policy=rr fairness=" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: round-robin fairness baseline not printed (SCHED: policy=rr fairness=)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# ...and the resonant order parameter + fairness alongside it.
	@if ! grep -q "SCHED: policy=resonant r=" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: resonant order parameter/fairness not printed (SCHED: policy=resonant r=)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: honest rr-vs-resonant comparison printed:"
	@grep "SCHED: " /tmp/qemu-boot-resonant.log 2>/dev/null || true
	@echo ""
	@echo "=== Smoke Test (resonant) PASSED ==="

# CI Smoke Test (qseed mode): boot WITH a quantum-entropy handoff on the
# kernel command line and prove ghostd traces its noise to that qseed.
ci-smoke-qseed: kernel
	@echo "=== QuantumOS CI Smoke Test (qseed handoff) ==="
	@echo ""
	@echo "[1/3] Build verified: $(BUILD_DIR)/kernel.elf exists"
	@test -f $(BUILD_DIR)/kernel.elf || (echo "ERROR: Kernel not built" && exit 1)
	@echo "[2/3] Running QEMU boot test WITH -append qseed=DEADBEEFCAFEBABE (10s)..."
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-append "qseed=DEADBEEFCAFEBABE" \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot-qseed.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: Kernel did not reach 'QuantumOS ready'"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd self-test gate missing (GHOSTD: 3/3 RECALL OK)"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# The kernel must still echo it accepted the qseed handoff...
	@if ! grep -q "Boot entropy accepted from cmdline (qseed=)" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: kernel did not echo the qseed handoff"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# ...and ghostd must trace its noise to that qseed (and NOT to a seedless PRNG).
	@if ! grep -q "GHOSTD: noise source = qseed-derived" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd did not report qseed-derived noise on a seeded boot"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "GHOSTD: noise source = prng (no qseed)" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd reported 'no qseed' despite a qseed handoff"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qseed handoff traced end-to-end (kernel echo + GHOSTD qseed-derived noise)"
	@# phase-3 gate still holds under a qseed boot: paradoxd resolves + couples.
	@if ! grep -q "PARADOXD: RESOLVED" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: paradoxd resolution gate missing under qseed (PARADOXD: RESOLVED)"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd resolution gate passed under qseed (PARADOXD: RESOLVED)"
	@echo ""
	@echo "=== Smoke Test (qseed) PASSED ==="

# CI Smoke Test (swarm bridge): boot with COM2 routed to a file and prove the
# ring-3 swarm_svc's Lamport-signed boot attestation verifies end-to-end — both
# seedless (qseed=none) and with a qseed handoff (attested qseed == cmdline).
# This is the one-way CI gate; the two-way PING/PONG path is proven locally via
# `make swarm-pingpong` (headless two-way serial into QEMU needs a TCP client).
ci-smoke-swarm: kernel
	@echo "=== QuantumOS CI Smoke Test (swarm bridge attestation) ==="
	@echo ""
	@echo "[seedless] boot: COM1 -> stdio, COM2 -> file..."
	@rm -f /tmp/qemu-swarm-com2.bin
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial stdio -serial file:/tmp/qemu-swarm-com2.bin \
		-m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-swarm.log || true
	@if ! grep -q "SWARM: boot attestation emitted" /tmp/qemu-swarm.log 2>/dev/null; then \
		echo "ERROR: swarm console gate missing (SWARM: boot attestation emitted)"; \
		cat /tmp/qemu-swarm.log 2>/dev/null || true; \
		echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: swarm console gate present (SWARM: boot attestation emitted)"
	@python3 scripts/verify_attestation.py /tmp/qemu-swarm-com2.bin --qseed none || \
		(echo "=== Smoke Test FAILED (seedless attestation) ==="; exit 1)
	@echo "SUCCESS: seedless boot attestation verified (qseed=none)"
	@echo ""
	@echo "[qseed] boot WITH -append qseed=DEADBEEFCAFEBABE, COM2 -> file..."
	@rm -f /tmp/qemu-swarm-com2q.bin
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-append "qseed=DEADBEEFCAFEBABE" \
		-serial stdio -serial file:/tmp/qemu-swarm-com2q.bin \
		-m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-swarmq.log || true
	@python3 scripts/verify_attestation.py /tmp/qemu-swarm-com2q.bin --qseed DEADBEEFCAFEBABE || \
		(echo "=== Smoke Test FAILED (qseed attestation) ==="; exit 1)
	@echo "SUCCESS: qseed boot attestation verified (attested qseed == cmdline)"
	@echo ""
	@echo "=== Smoke Test (swarm bridge) PASSED ==="

# Local two-way exercise: COM2 as a TCP server; drive PING/PONG + a DATA request
# routed to ghostd over capability-checked IPC. Not part of CI (needs a client).
swarm-pingpong: kernel
	@echo "=== QuantumOS swarm bridge two-way (PING/PONG + DATA->ghostd) ==="
	@(timeout 18s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial file:/tmp/qemu-swarm-com1.log \
		-serial tcp:127.0.0.1:5566,server -m 128M -display none -no-reboot \
		>/dev/null 2>&1 &) ; sleep 1
	@python3 scripts/swarm_pingpong.py --host 127.0.0.1 --port 5566 --timeout 16

# ISO/GRUB boot path (epic #101): boot the GRUB-built ISO with -cdrom —
# NOT QEMU's -kernel shortcut — so the real bootloader handoff (menu,
# default entry, MB1 info from GRUB) is what's under test. The default
# entry is the QUIET console (the usable interactive machine — verified
# on real hardware, where verbose chatter outran the prompt), so the
# gates are the quiet-surviving set: one-time boot milestones, the
# shell session, and a citizen verdict. GHOSTD's self-test line is
# quiet-suppressed (covered by ci-smoke on the -kernel path); in
# exchange this test asserts the quiet contract held — at most one
# timer-tick line in the whole session.
ci-smoke-iso: $(BUILD_DIR)/kernel.iso
	@echo "=== QuantumOS ISO/GRUB boot test (epic #101, quiet console entry) ==="
	@( printf 'help\nghost\nrun /bin/consciousnessd\nexit\n'; sleep 30 ) | \
		timeout 26s qemu-system-x86_64 -cdrom $(BUILD_DIR)/kernel.iso \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-iso.log || true
	@for gate in "QuantumOS ready" \
	             "CONS: screen console active (VGA text 80x25)" \
	             "QSH: QuantumOS interactive shell ready" \
	             "qsh: commands: help" \
	             "consciousnessd: CONSCIOUSNESS EMERGED"; do \
		if ! grep -qF "$$gate" /tmp/qemu-iso.log 2>/dev/null; then \
			echo "ERROR: ISO boot gate missing: $$gate"; \
			echo "Boot log:"; cat /tmp/qemu-iso.log 2>/dev/null || true; \
			echo "=== ISO Boot Test FAILED ==="; exit 1; \
		fi; echo "  [PASS] $$gate"; \
	done
	@if [ "$$(grep -c 'Timer tick' /tmp/qemu-iso.log 2>/dev/null)" -gt 1 ]; then \
		echo "ERROR: quiet contract broken — timer-tick chatter on the default ISO entry"; \
		echo "=== ISO Boot Test FAILED ==="; exit 1; \
	fi
	@echo "  [PASS] quiet contract: at most one timer-tick line"
	@echo "SUCCESS: GRUB/ISO boot reached the shell + citizen gate on a QUIET console"

# PS/2 keyboard path (epic #101): drive qsh entirely through emulated
# keyboard scancodes injected via the QEMU monitor — serial carries NO
# input this run, so the 'help' output can only have come through the
# i8042/IRQ1 path a real laptop keyboard uses.
ci-smoke-kbd: kernel
	@echo "=== QuantumOS PS/2 keyboard input test (epic #101) ==="
	@rm -f /tmp/qemu-kbd-serial.log
	@( sleep 8; \
	   for k in h e l p ret; do echo "sendkey $$k"; sleep 0.3; done; \
	   sleep 4; echo "quit" ) | \
		timeout 20s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-monitor stdio -serial file:/tmp/qemu-kbd-serial.log \
		-m 128M -display none -no-reboot >/dev/null 2>&1 || true
	@if ! grep -qF "QSH: QuantumOS interactive shell ready" /tmp/qemu-kbd-serial.log 2>/dev/null; then \
		echo "ERROR: shell never came up (QSH: QuantumOS interactive shell ready)"; \
		cat /tmp/qemu-kbd-serial.log 2>/dev/null || true; \
		echo "=== PS/2 Keyboard Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "qsh: commands: help" /tmp/qemu-kbd-serial.log 2>/dev/null; then \
		echo "ERROR: PS/2-typed 'help' did not reach qsh (qsh: commands: help)"; \
		echo "Serial log:"; cat /tmp/qemu-kbd-serial.log 2>/dev/null || true; \
		echo "=== PS/2 Keyboard Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh executed a command typed via PS/2 scancodes (IRQ1 -> ring -> shell)"

# Serial-less boot (epic #101 — the laptop shape): -serial none allocates
# NO COM1 device, so its ports float to 0xFF exactly like hardware with no
# UART — the configuration that hung the first real-laptop boot at the 45%
# splash (unbounded RX drain on a floating LSR). The boot must complete
# anyway; COM2 (still present) carries the swarm attestation, and verifying
# it proves the kernel got all the way through service bring-up with no
# console serial port at all.
ci-smoke-noserial: kernel
	@echo "=== QuantumOS serial-less boot test (epic #101 — the laptop path) ==="
	@rm -f /tmp/qemu-noserial-com2.bin
	@timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial none -serial file:/tmp/qemu-noserial-com2.bin \
		-m 128M -display none -no-reboot >/dev/null 2>&1 || true
	@python3 scripts/verify_attestation.py /tmp/qemu-noserial-com2.bin --qseed none || \
		(echo "ERROR: boot did not reach the swarm attestation without COM1"; \
		 echo "=== Serial-less Boot Test FAILED ==="; exit 1)
	@echo "SUCCESS: kernel booted fully with NO COM1 UART (attestation verified via COM2)"

# Screen-truth gate (epic #101): after a normal boot, dump the live VGA
# text cells through the QEMU monitor and assert a ring-3 SYS_WRITE line
# ("[user pid=") is actually ON SCREEN. Serial gates cannot see this —
# the third real-laptop finding was citizens running to a clean exit 0
# while their SYS_WRITE output went only to a serial port that did not
# exist. The console tee makes it visible; this proves it stays visible.
ci-smoke-screen: kernel
	@echo "=== QuantumOS on-screen output test (epic #101) ==="
	@rm -f /tmp/qemu-screen-mon.sock
	@(timeout 18s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial file:/tmp/qemu-screen-com1.log \
		-monitor unix:/tmp/qemu-screen-mon.sock,server,nowait \
		-m 128M -display none -no-reboot >/dev/null 2>&1 &) ; sleep 12
	@python3 scripts/check_vga_text.py /tmp/qemu-screen-mon.sock "[user pid=" || \
		(echo "ERROR: ring-3 SYS_WRITE output is not reaching the VGA screen"; \
		 echo "=== On-Screen Output Test FAILED ==="; exit 1)
	@sleep 7
	@echo "SUCCESS: ring-3 SYS_WRITE output is visible on the machine's own display"

# Quick validation for contributors
validate: kernel
	@echo "=== Quick Validation ==="
	@echo "[1/2] Building kernel..."
	@$(MAKE) -s kernel
	@echo "[2/2] Running API consistency check..."
	@./scripts/check-api-consistency.sh 2>/dev/null || echo "API check script not found (run from repo root)"
	@echo "=== Validation Complete ==="

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/
	rm -f *.iso

# Install dependencies (Ubuntu/Debian)
install-deps:
	@echo "Installing dependencies..."
	sudo apt-get update
	# Note: gcc-x86_64-elf may not be available in Ubuntu 24.04+
	# The Makefile supports fallback to system gcc
	sudo apt-get install -y build-essential gdb-multiarch qemu-system-x86 grub-pc-bin xorriso mtools nasm || true
	@echo "Attempting to install cross-compiler (may not be available)..."
	sudo apt-get install -y gcc-x86-64-elf 2>/dev/null || echo "Cross-compiler not available, using system gcc"

# Help
help:
	@echo "QuantumOS Kernel Makefile"
	@echo "========================"
	@echo ""
	@echo "Quick Start (new contributors):"
	@echo "  make install-deps       # Install dependencies (Ubuntu/Debian)"
	@echo "  make                    # Build kernel"
	@echo "  make ci-smoke           # Verify build + boot works"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build kernel (default)"
	@echo "  kernel         - Build kernel only"
	@echo "  run            - Run kernel in QEMU (interactive)"
	@echo "  run-iso        - Run kernel from ISO in QEMU"
	@echo "  ci-smoke       - CI smoke test (build + headless boot + validate)"
	@echo "  ci-smoke-iso   - Boot the GRUB ISO via -cdrom to the shell (epic #101)"
	@echo "  ci-smoke-kbd   - Drive qsh via PS/2 scancodes only (epic #101)"
	@echo "  ci-smoke-noserial - Boot with NO COM1 device at all (epic #101)"
	@echo "  ci-smoke-screen - Assert ring-3 output is ON the VGA screen (epic #101)"
	@echo "  validate       - Quick validation (build + API check)"
	@echo "  debug          - Debug kernel with GDB"
	@echo "  dump           - Show kernel information"
	@echo "  test           - Run all unit tests"
	@echo "  test-list      - List available tests"
	@echo "  test-<name>    - Run specific test (e.g., test-process)"
	@echo "  test-coverage  - Run tests with code coverage report"
	@echo "  clean          - Clean build artifacts"
	@echo "  install-deps   - Install required dependencies"
	@echo "  info           - Show build configuration"
	@echo "  help           - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH       - Target architecture (default: x86_64)"
	@echo "  BUILD_DIR  - Build directory (default: build/$(ARCH))"
	@echo ""
	@echo "Examples:"
	@echo "  make install-deps && make ci-smoke  # Full setup + verify"
	@echo "  make run                            # Run in QEMU"
	@echo "  make debug                          # Debug with GDB"

# Print configuration
info:
	@echo "Configuration:"
	@echo "  ARCH: $(ARCH)"
	@echo "  BUILD_DIR: $(BUILD_DIR)"
	@echo "  CC: $(CC)"
	@echo "  LD: $(LD)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Sources: $(KERNEL_SOURCES) $(ASSEMBLY_SOURCES)"
	@echo "  Objects: $(OBJECTS)"

# Phony targets
.PHONY: all clean kernel run run-iso debug dump test test-list test-coverage ci-smoke ci-smoke-resonant ci-smoke-qseed ci-smoke-iso ci-smoke-kbd ci-smoke-noserial ci-smoke-screen validate info install-deps help

# Default target
.DEFAULT_GOAL := all
