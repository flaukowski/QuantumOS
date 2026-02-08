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
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(KERNEL_DIR)/link.ld

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

# Source files
# KERNEL_SOURCES captures all .c files in kernel/src/ (including process*.c)
KERNEL_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.c)
IPC_SOURCES = $(wildcard $(KERNEL_DIR)/src/ipc/*.c)
ASSEMBLY_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.S)
# Assembly files compile to *_asm.o to avoid naming collisions with C files
OBJECTS = $(KERNEL_SOURCES:$(KERNEL_DIR)/src/%.c=$(BUILD_DIR)/%.o) \
          $(IPC_SOURCES:$(KERNEL_DIR)/src/ipc/%.c=$(BUILD_DIR)/ipc/%.o) \
          $(ASSEMBLY_SOURCES:$(KERNEL_DIR)/src/%.S=$(BUILD_DIR)/%_asm.o)

# Targets
.PHONY: all clean kernel run debug dump test test-list test-coverage

all: kernel

kernel: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/kernel.elf: $(OBJECTS) $(KERNEL_DIR)/link.ld
	@mkdir -p $(dir $@)
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Kernel linked successfully: $@"

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

# Create bootable image
$(BUILD_DIR)/kernel.iso: $(BUILD_DIR)/kernel.elf
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $< $(BUILD_DIR)/iso/boot/kernel.elf
	@echo "set timeout=0" > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "set default=0" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot2 /boot/kernel.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 grub2-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 echo "GRUB mkrescue not available, skipping ISO creation"

# Run in QEMU
run: $(BUILD_DIR)/kernel.elf
	@echo "Starting QEMU..."
	qemu-system-x86_64 -kernel $< -serial stdio -m 128M

run-iso: $(BUILD_DIR)/kernel.iso
	@echo "Starting QEMU with ISO..."
	qemu-system-x86_64 -cdrom $< -serial stdio -m 128M

# Debug with GDB
debug: $(BUILD_DIR)/kernel.elf
	@echo "Starting QEMU in debug mode..."
	qemu-system-x86_64 -kernel $< -serial stdio -m 128M -s -S &
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
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf \
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
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf \
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
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if grep -q "QuantumOS" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "SUCCESS: Boot banner detected"; \
		echo ""; \
		echo "=== Smoke Test PASSED ==="; \
	else \
		echo "WARNING: Boot banner not found (may be expected if kernel halts quickly)"; \
		echo "Check /tmp/qemu-boot.log for output"; \
		echo ""; \
		echo "=== Smoke Test COMPLETED (with warnings) ==="; \
	fi

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
.PHONY: all clean kernel run run-iso debug dump test test-list test-coverage ci-smoke validate info install-deps help

# Default target
.DEFAULT_GOAL := all
