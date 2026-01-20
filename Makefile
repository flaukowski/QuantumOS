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

# Default to debug build
CFLAGS += $(DEBUG_CFLAGS)

# Source files
# KERNEL_SOURCES captures all .c files in kernel/src/ (including process*.c)
KERNEL_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.c)
IPC_SOURCES = $(wildcard $(KERNEL_DIR)/src/ipc/*.c)
ASSEMBLY_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.S)
OBJECTS = $(KERNEL_SOURCES:$(KERNEL_DIR)/src/%.c=$(BUILD_DIR)/%.o) \
          $(IPC_SOURCES:$(KERNEL_DIR)/src/ipc/%.c=$(BUILD_DIR)/ipc/%.o) \
          $(ASSEMBLY_SOURCES:$(KERNEL_DIR)/src/%.S=$(BUILD_DIR)/%.o)

# Targets
.PHONY: all clean kernel run debug dump test

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

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/src/%.S
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

# Test kernel
test: kernel
	@echo "Running kernel tests..."
	@# TODO: Add kernel tests

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
	@echo "Targets:"
	@echo "  all        - Build kernel (default)"
	@echo "  kernel     - Build kernel only"
	@echo "  run        - Run kernel in QEMU"
	@echo "  run-iso    - Run kernel from ISO in QEMU"
	@echo "  debug      - Debug kernel with GDB"
	@echo "  dump       - Show kernel information"
	@echo "  test       - Run kernel tests"
	@echo "  clean      - Clean build artifacts"
	@echo "  install-deps - Install required dependencies"
	@echo "  help       - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH       - Target architecture (default: x86_64)"
	@echo "  BUILD_DIR  - Build directory (default: build/$(ARCH))"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build kernel"
	@echo "  make run                # Run in QEMU"
	@echo "  make debug              # Debug with GDB"
	@echo "  make ARCH=arm64 kernel  # Build for ARM64"

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
.PHONY: info install-deps

# Default target
.DEFAULT_GOAL := all
