name: Quantum Component
description: Report issues or request features for quantum-specific components
title: "[QUANTUM]: "
labels: ["quantum"]
body:
  - type: markdown
    attributes:
      value: |
        This template is for quantum-specific issues and features in QuantumOS. Please provide detailed information about the quantum component you're working with.

  - type: dropdown
    id: quantum-component
    attributes:
      label: Quantum Component
      description: Which quantum component is affected?
      options:
        - Qubit Manager
        - Coherence Tracker
        - Circuit Executor
        - Measurement Handler
        - Quantum Scheduler
        - Quantum HAL
        - Quantum Simulator
        - Quantum Hardware Interface
        - Error Correction
        - Other Quantum Component
    validations:
      required: true

  - type: dropdown
    id: hardware-type
    attributes:
      label: Hardware Type
      description: What type of quantum hardware is involved?
      options:
        - Simulator (Software)
        - Superconducting QPU
        - Ion Trap System
        - Photonic QPU
        - FPGA Accelerator
        - Neuromorphic Quantum
        - Multiple Types
        - Not Hardware-Specific
    validations:
      required: true

  - type: dropdown
    id: issue-type
    attributes:
      label: Issue Type
      description: What type of issue is this?
      options:
        - Bug Report
        - Feature Request
        - Performance Issue
        - Hardware Integration
        - Algorithm Improvement
        - Documentation
        - Security Issue
    validations:
      required: true

  - type: textarea
    id: description
    attributes:
      label: Description
      description: Detailed description of the quantum issue or feature
      placeholder: Provide detailed description...
    validations:
      required: true

  - type: textarea
    id: quantum-details
    attributes:
      label: Quantum-Specific Details
      description: Any quantum-specific technical details
      placeholder: |
        - Number of qubits involved:
        - Circuit depth:
        - Coherence time requirements:
        - Fidelity requirements:
        - Error rates:
        - Quantum algorithm used:
    validations:
      required: true

  - type: textarea
    id: reproduction-steps
    attributes:
      label: Reproduction Steps (if bug)
      description: Steps to reproduce the quantum issue
      placeholder: |
        1. Setup quantum context with N qubits
        2. Execute quantum circuit
        3. Observe unexpected behavior...

  - type: textarea
    id: expected-behavior
    attributes:
      label: Expected Quantum Behavior
      description: What quantum behavior did you expect?
      placeholder: Describe expected quantum behavior...

  - type: textarea
    id: actual-behavior
    attributes:
      label: Actual Quantum Behavior
      description: What actually happened?
      placeholder: Describe actual quantum behavior...

  - type: textarea
    id: quantum-metrics
    attributes:
      label: Quantum Performance Metrics
      description: Any relevant quantum performance data
      placeholder: |
        - Gate execution time:
        - Measurement fidelity:
        - Coherence preservation:
        - Error rates:
        - Circuit completion time:

  - type: dropdown
    id: severity
    attributes:
      label: Severity
      description: How severe is this issue?
      options:
        - Critical - Quantum data corruption or security vulnerability
        - High - Quantum computation failures
        - Medium - Quantum performance degradation
        - Low - Minor quantum issue
    validations:
      required: true

  - type: textarea
    id: environment
    attributes:
      label: Quantum Environment
      description: Information about your quantum development environment
      placeholder: |
        - Quantum simulator: [e.g., Qiskit, Cirq]
        - Quantum hardware: [if applicable]
        - Number of qubits: [simulated or physical]
        - Coherence time: [if applicable]
        - Error model: [if applicable]

  - type: textarea
    id: logs
    attributes:
      label: Quantum Logs and Output
      description: Relevant quantum logs, measurements, or output
      render: shell

  - type: textarea
    id: additional-context
    attributes:
      label: Additional Context
      description: Any other quantum-specific context
      placeholder: Add any other quantum context here...

  - type: checkboxes
    id: terms
    attributes:
      label: Confirmation
      description: Please confirm the following
      options:
        - label: I have searched existing quantum issues
          required: true
        - label: I have provided quantum-specific details
          required: true
        - label: This is not a security vulnerability (report privately if it is)
          required: true
