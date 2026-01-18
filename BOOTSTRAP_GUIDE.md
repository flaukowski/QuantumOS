# QuantumOS v0.1 Bootstrap Guide

## Quick Start

This guide covers building and running the QuantumOS v0.1 bootstrap kernel.

## Prerequisites

### Required Tools
- **Cross-compiler**: `gcc-x86_64-elf` (Ubuntu/Debian) or equivalent
- **QEMU**: For testing and debugging
- **GDB**: For debugging (multi-architecture version)
- **GRUB tools**: For creating bootable ISO (optional)

### Installation (Ubuntu/Debian)
```bash
# Install dependencies
make install-deps

# Or manually:
sudo apt-get update
sudo apt-get install gcc-x86_64-elf gdb-multiarch qemu-system-x86 grub-pc-bin xorriso
```

## Building the Kernel

### Basic Build
```bash
# Build kernel (debug build by default)
make

# Show build configuration
make info

# Clean build artifacts
make clean
```

### Build Variants
```bash
# Debug build (default)
make CFLAGS="-g -O0"

# Release build
make CFLAGS="-O2 -DNDEBUG"

# Custom build
make CFLAGS="-g -O1 -DDEBUG_KERNEL"
```

## Running the Kernel

### QEMU Direct Boot
```bash
# Run kernel directly
make run

# Or manually:
qemu-system-x86_64 -kernel build/x86_64/kernel.elf -serial stdio -m 128M
```

### QEMU ISO Boot
```bash
# Create and run ISO
make run-iso

# Or manually:
make build/x86_64/kernel.iso
qemu-system-x86_64 -cdrom build/x86_64/kernel.iso -serial stdio -m 128M
```

### Debug Mode
```bash
# Start QEMU in debug mode and connect GDB
make debug

# Manual debugging:
qemu-system-x86_64 -kernel build/x86_64/kernel.elf -serial stdio -m 128M -s -S &
gdb-multiarch build/x86_64/kernel.elf -ex "target remote localhost:1234" -ex "break kernel_main" -ex "continue"
```

## Kernel Features (v0.1)

### Implemented Components
- ✅ **Multiboot2 Boot** - Standard bootloader support
- ✅ **Basic Console** - Serial output via COM1
- ✅ **Memory Management** - Physical and virtual memory setup
- ✅ **Interrupt System** - IDT, exception handlers, IRQ routing
- ✅ **GDT Setup** - Global descriptor table for x86_64
- ✅ **Cross-Compilation** - Full build system

### Boot Sequence
1. **Firmware** → **Bootloader** → **Kernel Entry**
2. **Assembly Setup** → **C Initialization**
3. **HAL Init** → **Memory Init** → **Interrupts Init**
4. **Core Services** → **User Space** (future)

### Current Output
```
[BOOT] QuantumOS v0.1 booting...
[BOOT] Multiboot information validated
[BOOT] Starting early initialization...
[BOOT] Early initialization complete
[BOOT] Starting kernel initialization...
[BOOT] Initializing HAL...
[BOOT] HAL initialization complete
[BOOT] Initializing memory management...
[BOOT] Physical memory manager initialized
[BOOT] Total frames: [number]
[BOOT] Free frames: [number]
[BOOT] Virtual memory manager initialized
[BOOT] Kernel heap initialized
[BOOT] Memory management initialization complete
[BOOT] Initializing interrupt system...
[BOOT] Interrupt system initialized
[BOOT] Initializing core services...
[BOOT] Core services initialization complete
[BOOT] Kernel initialization complete
[BOOT] QuantumOS ready
```

## Development Workflow

### Adding New Features
1. **Design** - Add to documentation
2. **Headers** - Define interfaces in `kernel/include/`
3. **Implementation** - Add code to `kernel/src/`
4. **Testing** - Add tests and verify
5. **Integration** - Update main initialization

### Debugging Tips
```bash
# Dump kernel information
make dump

# View disassembly
objdump -d build/x86_64/kernel.elf | less

# Debug with GDB
gdb-multiarch build/x86_64/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
(gdb) info registers
(gdb) bt
```

### Common Issues

#### Build Failures
```bash
# Check cross-compiler installation
x86_64-elf-gcc --version

# Clean and rebuild
make clean && make

# Check dependencies
make install-deps
```

#### Boot Failures
```bash
# Check multiboot header
readelf -h build/x86_64/kernel.elf

# Verify entry point
objdump -f build/x86_64/kernel.elf

# Debug with QEMU monitor
qemu-system-x86_64 -kernel build/x86_64/kernel.elf -monitor stdio
```

#### Memory Issues
```bash
# Check memory layout
readelf -l build/x86_64/kernel.elf

# Debug page tables
(gdb) x/16x 0x100000  # View PML4 table
```

## Architecture Support

### Current Status
- **x86_64**: ✅ Fully supported
- **ARM64**: 🔄 Planned
- **RISC-V**: 🔄 Planned

### Adding New Architectures
1. **Create architecture directory**: `kernel/hal/<arch>/`
2. **Add assembly boot code**: `boot.S`
3. **Implement HAL functions**: `hal.c`
4. **Update Makefile**: Add architecture rules
5. **Test**: Verify boot and basic functionality

## Next Steps

### v0.1 Completion
- [ ] Fix any remaining boot issues
- [ ] Add basic process structures
- [ ] Implement capability system foundation
- [ ] Add quantum resource management
- [ ] Create basic IPC system

### v0.2 Planning
- [ ] User-space services
- [ ] Quantum scheduler
- [ ] Device drivers
- [ ] Filesystem support

## Contributing

### Code Style
- Use kernel coding style (Linux-like)
- 4-space indentation, no tabs
- 80-character line limit
- Function comments in Doxygen format

### Testing
- Test all new features
- Verify boot on QEMU
- Check for memory leaks
- Update documentation

### Submission
- Create feature branch
- Add tests
- Update docs
- Submit pull request

## Resources

### Documentation
- [Kernel Roadmap](docs/KERNEL_ROADMAP.md)
- [Microkernel Design](docs/MICROKERNEL_DESIGN.md)
- [Memory Management](docs/MEMORY_DESIGN.md)
- [Interrupt System](docs/INTERRUPT_DESIGN.md)

### External References
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [x86_64 Architecture Manual](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html)
- [OSDev Wiki](https://wiki.osdev.org/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)

### Community
- GitHub Issues: Report bugs and request features
- Discussions: Architecture and design questions
- Wiki: Additional documentation and tutorials

## Troubleshooting

### Boot Loop
- Check multiboot header alignment
- Verify GDT setup
- Check stack pointer initialization

### Memory Corruption
- Validate page table mappings
- Check heap boundaries
- Verify stack usage

### Interrupt Issues
- Confirm IDT setup
- Check PIC initialization
- Verify interrupt handler registration

### Performance Issues
- Profile with GDB
- Check for infinite loops
- Optimize critical paths

This bootstrap implementation provides a solid foundation for building the complete QuantumOS with quantum-aware features and microkernel architecture.
