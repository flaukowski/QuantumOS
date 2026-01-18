# PRad: Stacked Cognitive-Quantum Operating Architecture

## QuantumOS + Singularis Prime

**Status:** Active Research + Open Engineering

**Licensing:**
- QuantumOS: GPL v2
- Singularis Prime: MIT
- MSI: MIT / dual-license recommended

---

## 0. Executive Thesis

Operating systems are physical.
Cognition is abstract.
They must not be conflated—but they must align.

This document defines a stacked architecture where:

- **QuantumOS** is the physical, temporal, and legal ground
- **Singularis Prime (SP)** is the cognitive, symbolic, and agentic layer
- **MSI (Minimal Substrate Interface)** is the binding contract between them

This stack enables:
- neuromorphic hardware
- quantum accelerators
- AI-native execution
- cognitive agents
- future OS research

without sacrificing rigor, freedom, or clarity of responsibility.

---

## 1. Scope

### This PRad defines:
- Layer boundaries
- Responsibilities per layer
- Control flow and boot flow
- Execution semantics
- Licensing separation
- Evolution path from today's SP-on-Android → tomorrow's bare-metal QuantumOS

### This PRad does not:
- redefine Singularis Prime
- replace the existing MSI spec
- require immediate bare-metal implementation

---

## 2. Layered Architecture (Authoritative)

```
┌───────────────────────────────────────────────┐
│ Singularis Prime Cognitive Programs           │
│-----------------------------------------------│
│ • Agents                                      │
│ • Domains                                     │
│ • Lanes                                       │
│ • Cognitive primitives                        │
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ Shinobi.Substrate (SP Kernel Module)          │
│-----------------------------------------------│
│ • SP → MSI lowering                           │
│ • Policy expression                           │
│ • Cognitive scheduling hints                  │
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ MSI - Minimal Substrate Interface             │
│-----------------------------------------------│
│ • Execution lanes                             │
│ • Events                                      │
│ • State (addressable + associative)           │
│ • Domains & capabilities                      │
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ QuantumOS User-Space Services                 │
│-----------------------------------------------│
│ • Classical scheduler                         │
│ • Quantum scheduler                           │
│ • Memory & entropy manager                    │
│ • Device services                             │
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ QuantumOS Microkernel (GPL v2)                │
│-----------------------------------------------│
│ • IPC                                         │
│ • Capabilities                                │
│ • Interrupts                                  │
│ • Memory protection                           │
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ Hardware / Firmware / Physics                 │
└───────────────────────────────────────────────┘
```

---

## 3. Clear Responsibility Split (Non-Negotiable)

### QuantumOS Owns
- Time
- Safety
- Memory protection
- Hardware truth
- Interrupts
- Physical qubits / accelerators
- Legal copyleft obligations (GPL)

### Singularis Prime Owns
- Meaning
- Cognition
- Agency
- Symbolic state
- Policy expression
- Developer-facing abstraction
- Rapid evolution (MIT)

### MSI Owns
- The truce
- The boundary
- The guarantees

---

## 4. MSI as the Binding Contract

**MSI is the only interface SP is allowed to see.**

SP never calls:
- Linux syscalls
- Android APIs
- QuantumOS kernel APIs directly

Instead:
```
SP  →  Shinobi.Substrate  →  MSI  →  QuantumOS services
```

This ensures:
- portability
- testability
- long-term survivability

### MSI Guarantees (Recap)
- Execution lanes
- Event system
- State (addressable + associative)
- Domains (capabilities)
- Time source (monotonic or event-based)

**QuantumOS must implement MSI faithfully.**

---

## 5. Execution Semantics Across the Stack

### Lane Mapping

| SP Concept | MSI | QuantumOS |
|------------|-----|-----------|
| Lane | Execution lane | Thread / task |
| Lane policy | Scheduling hint | Scheduler input |
| Yield | lane.yield() | Cooperative preemption |
| Sleep | lane.sleep() | Timer interrupt |

**QuantumOS may ignore hints, but must never violate safety.**

---

## 6. Domains as the Security Spine

SP Domains map directly to QuantumOS capability sets.

- `domain.grant()` → capability issuance
- `domain.seal()` → irrevocable revocation

- No ambient authority
- No global namespace

This makes SP:
- secure by construction
- auditable
- compatible with microkernel philosophy

---

## 7. Memory & State Model

### Classical Memory
- Owned by QuantumOS
- Protected, paged, isolated

### Associative State
- Exposed via MSI
- Backed by:
  - RAM
  - NPU memory
  - persistent object stores
  - quantum result archives

**SP treats associative state as cognitive memory, not RAM.**

---

## 8. Quantum Semantics (Critical Boundary)

### QuantumOS Responsibilities
- Qubit allocation
- Coherence windows
- Measurement
- Error rates
- Physical scheduling

### Singularis Prime Responsibilities
- Express intent
- Handle probabilistic results
- Adapt behavior
- Never assume determinism

**SP never manipulates qubits directly.**
It orchestrates meaning, not physics.

---

## 9. Boot & Evolution Path

### Phase A (Today - Exists)
- SP running on Android
- MSI implemented via Kotlin / NDK
- QuantumOS simulated or absent

### Phase B (Near-Term)
- QuantumOS services running in user space
- MSI backed by POSIX/Linux initially
- SP unchanged

### Phase C (Mid-Term)
- QuantumOS microkernel boots bare metal
- MSI implemented natively
- SP becomes first-class cognitive environment

### Phase D (Long-Term)
- Quantum accelerators
- Neuromorphic substrates
- Distributed cognitive clusters

---

## 10. Licensing Strategy (Intentional)

| Layer | License | Rationale |
|-------|---------|-----------|
| QuantumOS kernel | GPL v2 | Protects the commons |
| QuantumOS services | GPL v2 | Prevents enclosure |
| MSI spec | MIT / dual | Maximum adoption |
| Singularis Prime | MIT | Rapid evolution |
| SP programs | Any | User freedom |

This preserves:
- openness at the bottom
- creativity at the top

---

## 11. Success Criteria

This stack is successful if:
- SP code runs unchanged across substrates
- QuantumOS can evolve without breaking SP
- MSI remains small and stable
- Contributors understand where to build
- The system survives hype cycles

---

## 12. Final Statement

> **QuantumOS makes time safe.**
> **Singularis Prime makes time meaningful.**
> **MSI keeps them honest.**

This is not a monolith.
It is a living stack.
