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
else
    # Fall back to system GCC with appropriate flags for freestanding code
    CC = gcc
    LD = ld
    OBJCOPY = objcopy
    OBJDUMP = objdump
endif
GDB = gdb-multiarch

# Compiler flags
CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -mno-red-zone \
         -mno-mmx -mno-sse -mno-sse2 -fno-omit-frame-pointer \
         -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel \
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
USER_PROGS = init echo client hbsvc ghostd ghost_test paradoxd paradox_test swarm_svc
USER_ELF_OBJS = $(USER_PROGS:%=$(USER_BUILD)/%_elf.o)

OBJECTS = $(KERNEL_SOURCES:$(KERNEL_DIR)/src/%.c=$(BUILD_DIR)/%.o) \
          $(IPC_SOURCES:$(KERNEL_DIR)/src/ipc/%.c=$(BUILD_DIR)/ipc/%.o) \
          $(RESONANCE_SOURCES:$(KERNEL_DIR)/src/resonance/%.c=$(BUILD_DIR)/resonance/%.o) \
          $(ASSEMBLY_SOURCES:$(KERNEL_DIR)/src/%.S=$(BUILD_DIR)/%_asm.o) \
          $(USER_ELF_OBJS)

# Targets
.PHONY: all clean kernel run debug dump test test-list test-coverage ci-smoke ci-smoke-resonant ci-smoke-qseed ci-smoke-swarm swarm-pingpong

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

$(USER_BUILD)/%.elf: $(USER_DIR)/%.c $(USER_DIR)/user.ld $(USER_DIR)/usys.h $(USER_DIR)/ghost.h $(USER_DIR)/paradox.h $(USER_DIR)/sha256.h $(USER_DIR)/swarm.h
	@mkdir -p $(USER_BUILD)
	@echo "Building user program: $<..."
	$(CC) $(USER_CFLAGS) -T $(USER_DIR)/user.ld -o $@ $<

# Wrap each ELF as an object. Run objcopy from inside the build dir so the
# generated symbols are _binary_<name>_elf_{start,end,size}.
$(USER_BUILD)/%_elf.o: $(USER_BUILD)/%.elf
	cd $(USER_BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		$*.elf $*_elf.o

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Assembly files compile to *_asm.o to avoid collision with C files of same name
$(BUILD_DIR)/%_asm.o: $(KERNEL_DIR)/src/%.S
	@mkdir -p $(dir $@)
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

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

# Create bootable image (GRUB's multiboot v1 loader also wants ELF32)
$(BUILD_DIR)/kernel.iso: $(BUILD_DIR)/kernel.elf32
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $< $(BUILD_DIR)/iso/boot/kernel.elf
	@echo "set timeout=0" > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "set default=0" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "insmod all_video" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "set gfxpayload=1024x768x32" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 grub2-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 echo "GRUB mkrescue not available, skipping ISO creation"

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
	@echo "[2/3] Running QEMU boot test (10 second timeout)..."
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
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
	@echo ""
	@echo "=== Smoke Test PASSED ==="

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
	sudo apt-get install -y build-essential gdb-multiarch qemu-system-x86 grub-pc-bin xorriso nasm || true
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
.PHONY: all clean kernel run run-iso debug dump test test-list test-coverage ci-smoke ci-smoke-resonant ci-smoke-qseed validate info install-deps help

# Default target
.DEFAULT_GOAL := all
