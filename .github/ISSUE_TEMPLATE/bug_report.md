name: Bug Report
description: File a bug report to help us improve QuantumOS
title: "[BUG]: "
labels: ["bug"]
body:
  - type: markdown
    attributes:
      value: |
        Thanks for taking the time to fill out this bug report! Please provide as much detail as possible.

  - type: textarea
    id: bug-description
    attributes:
      label: Bug Description
      description: A clear and concise description of what the bug is
      placeholder: Describe the bug...
    validations:
      required: true

  - type: textarea
    id: reproduction-steps
    attributes:
      label: Reproduction Steps
      description: Steps to reproduce the behavior
      placeholder: |
        1. Build the kernel with 'make'
        2. Run with 'make run'
        3. Observe error...
    validations:
      required: true

  - type: textarea
    id: expected-behavior
    attributes:
      label: Expected Behavior
      description: What you expected to happen
      placeholder: Describe the expected behavior...
    validations:
      required: true

  - type: textarea
    id: actual-behavior
    attributes:
      label: Actual Behavior
      description: What actually happened
      placeholder: Describe what actually happened...
    validations:
      required: true

  - type: textarea
    id: system-info
    attributes:
      label: System Information
      description: Information about your development environment
      placeholder: |
        - OS: [e.g., Ubuntu 22.04]
        - Architecture: [e.g., x86_64]
        - Compiler: [e.g., gcc-x86_64-elf 11.2]
        - QEMU version: [e.g., 7.0]
        - Git commit: [e.g., abc123]
    validations:
      required: true

  - type: dropdown
    id: component
    attributes:
      label: Component
      description: Which component is affected?
      options:
        - Kernel
        - Memory Management
        - Interrupt System
        - Process Management
        - IPC System
        - Capability System
        - Quantum Resources
        - User-Space Services
        - Build System
        - Documentation
        - Other
    validations:
      required: true

  - type: dropdown
    id: severity
    attributes:
      label: Severity
      description: How severe is this bug?
      options:
        - Critical - System crash or security vulnerability
        - High - Major functionality broken
        - Medium - Partial functionality affected
        - Low - Minor issue or cosmetic problem
    validations:
      required: true

  - type: textarea
    id: logs
    attributes:
      label: Logs and Output
      description: Relevant logs, error messages, or output
      render: shell

  - type: textarea
    id: additional-context
    attributes:
      label: Additional Context
      description: Any other context about the problem
      placeholder: Add any other context here...

  - type: checkboxes
    id: terms
    attributes:
      label: Confirmation
      description: Please confirm the following
      options:
        - label: I have searched existing issues for similar bugs
          required: true
        - label: I have provided all requested information
          required: true
        - label: This is not a security vulnerability (security issues should be reported privately)
          required: true
