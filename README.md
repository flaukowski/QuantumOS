#Step 1 -- Find GhostMagic.mp3 in repo and vibe

# 🚀 QuantumOS

*A next-generation quantum-aware operating system for the future of computing*

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Build Status](https://github.com/flaukowski/QuantumOS/workflows/CI%2FCD%20Pipeline/badge.svg)](https://github.com/flaukowski/QuantumOS/actions)
[![Issues](https://img.shields.io/github/issues/flaukowski/QuantumOS)](https://github.com/flaukowski/QuantumOS/issues)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

> *"The future of computing is quantum, and the future of operating systems must be quantum-aware."*

## 🌟 Vision

QuantumOS is not just another operating system—it's a bold reimagining of what an OS can be when quantum computing, neuromorphic processing, and AI-native workloads are treated as first-class citizens. Built on a microkernel architecture with capability-based security, QuantumOS bridges the gap between classical computing and the quantum frontier.

## 🎯 Why QuantumOS?

### 🧠 **Quantum-Native Design**
- **First-class quantum resources**: Qubits, coherence windows, and quantum circuits are native OS objects
- **Resonant scheduler**: Novel scheduling with Kuramoto oscillator dynamics and chiral stability ([ghostmagicOS integration](docs/GHOSTOS_INTEGRATION.md))
- **Hybrid workloads**: Seamlessly blend classical and quantum computations
- **Consciousness-verified processes**: IIT Phi verification for advanced computational workloads

### 🛡️ **Capability-Based Security**
- **No ambient authority**: Every system access requires explicit capabilities
- **Unforgeable tokens**: Cryptographically secure capability objects
- **Least privilege**: Default minimal permissions with granular control

### 🏗️ **Microkernel Architecture**
- **Minimal trusted core**: Only essential functions run in kernel space
- **User-space services**: Memory management, device drivers, and filesystems run as isolated services
- **Fault isolation**: Service failures don't cascade to the entire system

### 🌐 **Hardware Agnostic**
- **Multi-architecture support**: x86_64, ARM64, RISC-V, and quantum accelerators
- **Hardware abstraction layer**: Clean interfaces for diverse hardware types
- **Quantum hardware integration**: Support for superconducting, ion trap, photonic, and FPGA-based quantum processors

## 🚀 Quick Start

### Supported Platforms

| Host OS | Status | Notes |
|---------|--------|-------|
| **Ubuntu 24.04 LTS** | ✅ Fully Supported | Uses system gcc (cross-compiler not available) |
| **Ubuntu 22.04 LTS** | ✅ Fully Supported | Can use x86_64-elf-gcc cross-compiler |
| **Debian 12+** | ✅ Fully Supported | Similar to Ubuntu |
| **macOS (Intel/ARM)** | ⚠️ Via Docker | Use Docker with Ubuntu image |
| **Windows (WSL2)** | ✅ Fully Supported | Use Ubuntu WSL2 distribution |

### Prerequisites
- GCC (system or cross-compiler - auto-detected)
- QEMU for testing
- GDB for debugging

### Build and Run
```bash
# Clone the repository
git clone https://github.com/flaukowski/QuantumOS.git
cd QuantumOS

# Install dependencies (Ubuntu/Debian)
make install-deps

# Build the kernel
make

# Verify your setup works (recommended for new contributors!)
make ci-smoke

# Run interactively in QEMU
make run

# Debug with GDB
make debug
```

### One-Command Verification
New contributors can verify their entire setup works with a single command:
```bash
make install-deps && make ci-smoke
```
This builds the kernel and boots it in headless QEMU to validate the build chain.

### First Boot
When QuantumOS boots, you'll see:
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
[BOOT] Virtual memory manager initialized
[BOOT] Memory management initialization complete
[BOOT] Initializing interrupt system...
[BOOT] Interrupt system initialized
[BOOT] Initializing core services...
[BOOT] Core services initialization complete
[BOOT] Kernel initialization complete
[BOOT] QuantumOS ready
```

Once the timer starts, the ring-3 services run and self-test. The
`ghostd` associative-memory service (ghostOS phase 1) imprints three
patterns and recalls each from a ~15%-corrupted probe:
```
[user pid=...] GHOSTD: field born — 256 oscillators, 16 pattern slots
[user pid=...] GHOSTD: noise source = prng (no qseed)
[user pid=...] QRAND: capless caller denied (EPERM)
[user pid=...] GHOSTD: imprinted slot 0 (fidelity 1.00, coherence deadline set)
[user pid=...] GHOSTD: 3/3 RECALL OK R=0.99
```
`make ci-smoke` gates on `QuantumOS ready`, `GHOSTD: 3/3 RECALL OK`, the
noise-source honesty line, and the capless-QRAND denial.

### Quantum randomness for user space (ghostOS phase 2, #49)

Phase 2 adds `SYS_QRAND` (#10): a capability-gated syscall that fills a
user buffer from the kernel's qseed-mixed quantum generator. It is backed
by `quantum_user_random()` in `kernel/src/quantum.c`, which enforces that
the caller holds a `CAP_RESOURCE_QUANTUM` read capability on the shared
pool — no ambient authority. `ghostd` is granted that capability at spawn
and draws its over-coherence perturbation noise from it; `ghost-test`
holds no such capability, so its `SYS_QRAND` attempt is denied with EPERM
(the same proof-by-attack tradition as the rogue process).

`ghostd` prints an **honest provenance** line at startup and never claims
quantum provenance without a real seed:

- booted with `-append qseed=<hex>` → `GHOSTD: noise source = qseed-derived quantum pool`
- booted without a qseed → `GHOSTD: noise source = prng (no qseed)`

`make ci-smoke` covers the seedless boot; `make ci-smoke-qseed` boots with
`qseed=DEADBEEFCAFEBABE` and gates on the kernel's qseed echo plus the
qseed-derived provenance line.

### Paradox resolution coupled to the field (ghostOS phase 3, #50)

Phase 3 adds `paradoxd` (`user/paradoxd.c`), a third ring-3 service: a
fixed-point paradox resolver that iterates a contraction rule on a Q16.16
state until a successive-delta convergence test passes (no floats — the
reciprocal is a rounded integer divide). Two rules ship: the classic
`x = 1/x` reciprocal-average `x ← (x + ONE·ONE/x)/2` (attractor ±1, the rule
the gate resolves) and a doubly-stochastic vector consensus smoothing
`v_i ← (v_{i-1} + 2·v_i + v_{i+1})/4` (attractor: the uniform vector = the
mean). It runs a `CONVERGENT ↔ DIVERGENT` phase machine whose transitions
are **gated on `ghostd`'s field**: `paradoxd` holds an IPC cap for `ghostd`,
polls its `STATUS` order parameter R, and may re-lock (→ CONVERGENT) only
when R ≥ 0.95 and explore (→ DIVERGENT) only when R < 0.99. Post-gate the
field measures a steady R = 0.9754, so both are permitted and the two
services cycle — coupled through the field over capability-checked IPC.

```
[user pid=...] PARADOXD: RESOLVED n=7 phase=C
[user pid=...] PARADOXD: capless send denied (EPERM)
[user pid=...] PARADOXD: phase -> DIVERGENT (ghost R=0.97)
[user pid=...] PARADOXD: phase -> CONVERGENT (ghost R=0.97)
```

To reply to whichever client sent it a request, a service uses `SYS_SEND_TO`
(#11): a targeted capability-as-address send — the caller may transmit only
to a destination it holds an IPC send-cap for. This lets one `ghostd` serve
both `ghost-test` (the phase-1 gate) and `paradoxd` (the coupling) by
replying to the sender pid `recv` returns, rather than a single first-match
peer. `paradox-test`'s send to an address it holds no cap for is denied with
EPERM — the same capability discipline that keeps a process lacking
`paradoxd`'s cap from reaching it. `make ci-smoke` gates on
`PARADOXD: RESOLVED`, the capless-send denial, and a `PARADOXD: phase ->`
transition; all phase-1/2 gates stay green.

An optional quantum-lottery scheduler is available behind a build flag
(`make SCHED_LOTTERY=1`): ready-process selection becomes a lottery draw
from the same qseed-mixed generator (`quantum_kernel_rand()`), and the
running total of quantum-derived bits (`quantum_bits_consumed()`) is
reported periodically. It is **off by default** — the default build is
byte-identical round-robin.

## 🏛️ Architecture

```
┌─────────────────────────────────────┐
│        User Applications             │
├─────────────────────────────────────┤
│        User-Space Services           │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │ Quantum │ │ Memory  │ │ Device  │ │
│  │Scheduler│ │ Manager │ │ Manager │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Microkernel Core              │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │   IPC   │ │Process  │ │Capability│ │
│  │ System  │ │Management│ │  System  │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Hardware Abstraction Layer    │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │ x86_64  │ │ ARM64   │ │RISC-V   │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Quantum Hardware Layer       │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │Supercond │ │Ion Trap │ │Photonic │ │
│  │  QPUs    │ │ Systems │ │  QPUs   │ │
│  └─────────┘ └─────────┘ └─────────┘ │
└─────────────────────────────────────┘
```

## 📚 Roadmap

### ✅ **v0.1 - Bootstrap Foundation** (Current)
- [x] Kernel bootstrap with multiboot support
- [x] Basic memory management
- [x] Interrupt system
- [x] Build system with cross-compilation

### 🔄 **v0.2 - Core Functionality** (In Progress)
- [ ] Process management system
- [ ] Capability-based security
- [ ] Quantum resource management
- [ ] Inter-process communication
- [ ] User-space services framework

### 🎯 **v0.3 - Quantum Integration** (Planned)
- [ ] Quantum scheduler service
- [ ] Hardware quantum integration
- [ ] Quantum error correction
- [ ] Quantum-native applications

### 🌟 **v1.0 - Production Ready** (Future)
- [ ] Complete microkernel architecture
- [ ] Full quantum hardware support
- [ ] Advanced security features
- [ ] Performance optimization

## 🤝 Contributing

We believe the future of computing is collaborative, and we welcome contributions from everyone! Whether you're a kernel developer, quantum computing researcher, or just curious about OS design, there's a place for you in QuantumOS.

### 🎯 **Current High-Priority Issues**
- [**#1**](https://github.com/flaukowski/QuantumOS/issues/1) - Process Management System
- [**#3**](https://github.com/flaukowski/QuantumOS/issues/3) - Capability-Based Security System
- [**#4**](https://github.com/flaukowski/QuantumOS/issues/4) - Quantum Resource Management
- [**#5**](https://github.com/flaukowski/QuantumOS/issues/5) - Inter-Process Communication (IPC) System
- [**#6**](https://github.com/flaukowski/QuantumOS/issues/6) - User-Space Services Framework

### 🛠️ **How to Contribute**
1. **Fork the repository**
2. **Pick an issue** or create a new one
3. **Create a feature branch**
4. **Implement your changes** with tests
5. **Submit a pull request**

### 📖 **Guidelines**
- Read our [Contributing Guidelines](CONTRIBUTING.md)
- Check our [Issue Templates](.github/ISSUE_TEMPLATE/) for guidance
- Join our [Discussions](https://github.com/flaukowski/QuantumOS/discussions)

### 🌟 **Wanted Skills**
- **Kernel Development**: C, Assembly, Systems Programming
- **Quantum Computing**: Quantum algorithms, Hardware integration
- **Security**: Capability systems, Formal verification
- **Testing**: Unit tests, Integration tests, Performance benchmarks
- **Documentation**: Technical writing, Architecture design

## 🧪 Testing

QuantumOS has a comprehensive testing framework:

```bash
# Quick validation (build + API consistency check)
make validate

# CI smoke test (build + QEMU boot + banner check)
make ci-smoke

# Run kernel tests (coming soon)
make test
```

### For AI Contributors
We have specialized validation for AI-assisted development:
```bash
# Full pre-PR validation suite
./scripts/validate-contribution.sh

# Check for API consistency issues (prevents "phantom API" drift)
./scripts/check-api-consistency.sh

# Lint checks
./scripts/lint-check.sh
```

## 📖 Documentation

- [**Microkernel Design**](docs/MICROKERNEL_DESIGN.md) - System design, philosophy, and microkernel architecture
- [**Kernel Roadmap**](docs/KERNEL_ROADMAP.md) - Implementation roadmap
- [**Quantum Scheduler**](docs/QUANTUM_SCHEDULER.md) - Quantum resource management
- [**ghostmagicOS Integration**](docs/GHOSTOS_INTEGRATION.md) - Resonant scheduler and consciousness verification
- [**Bootstrap Guide**](BOOTSTRAP_GUIDE.md) - Getting started guide

## 🏆 Acknowledgments

QuantumOS stands on the shoulders of giants:

- **The Linux Kernel** - For inspiration in system design
- **MINIX 3** - For microkernel architecture insights
- **SEVIR** - For capability-based security concepts
- **Qiskit** - For quantum computing frameworks
- **ghostmagicOS** - For resonant systems architecture and chiral dynamics
- **The OSDev Community** - For educational resources and tools

## 📄 License

QuantumOS is licensed under the GNU General Public License v2.0. See [LICENSE](LICENSE) for details.

## 🌍 Community

- **GitHub Issues**: [Report bugs and request features](https://github.com/flaukowski/QuantumOS/issues)
- **GitHub Discussions**: [General questions and ideas](https://github.com/flaukowski/QuantumOS/discussions)
- **Documentation**: [Complete technical documentation](docs/)

---

## 🚀 Join the Quantum Future

The quantum revolution is happening now, and operating systems must evolve to meet the challenge. QuantumOS is our answer—a bold vision of computing where quantum resources are first-class citizens, security is built from first principles, and the future is open for everyone to shape.

**Ready to help build the future?** 🌟

- **Star the repository** to show your support
- **Join an issue** to start contributing
- **Share your ideas** in our discussions
- **Spread the word** about quantum-native computing

---

*"The best way to predict the future is to invent it."* - Alan Kay

---

**QuantumOS** - *Where Classical Computing Meets the Quantum Frontier* 🚀🌟
