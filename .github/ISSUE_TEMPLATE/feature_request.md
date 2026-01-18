name: Feature Request
description: Suggest an idea for QuantumOS
title: "[FEATURE]: "
labels: ["enhancement"]
body:
  - type: markdown
    attributes:
      value: |
        Thanks for suggesting a feature for QuantumOS! Please provide as much detail as possible.

  - type: textarea
    id: feature-description
    attributes:
      label: Feature Description
      description: A clear and concise description of the feature you'd like to see
      placeholder: Describe the feature...
    validations:
      required: true

  - type: textarea
    id: problem-statement
    attributes:
      label: Problem Statement
      description: What problem does this feature solve?
      placeholder: What problem would this feature solve?
    validations:
      required: true

  - type: textarea
    id: proposed-solution
    attributes:
      label: Proposed Solution
      description: How do you envision this feature working?
      placeholder: Describe how you think this should work...
    validations:
      required: true

  - type: textarea
    id: alternatives
    attributes:
      label: Alternatives Considered
      description: What other approaches have you considered?
      placeholder: Describe any alternative solutions you've considered...

  - type: dropdown
    id: component
    attributes:
      label: Component
      description: Which component would this feature affect?
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
    id: priority
    attributes:
      label: Priority
      description: How important is this feature?
      options:
        - Critical - Essential for v1.0
        - High - Important for next release
        - Medium - Nice to have
        - Low - Future consideration
    validations:
      required: true

  - type: dropdown
    id: complexity
    attributes:
      label: Complexity
      description: How complex do you think this feature is to implement?
      options:
        - Low - Simple change
        - Medium - Moderate complexity
        - High - Complex implementation
        - Very High - Requires major architectural changes
    validations:
      required: true

  - type: textarea
    id: use-cases
    attributes:
      label: Use Cases
      description: How would you use this feature?
      placeholder: |
        - Use case 1: Description
        - Use case 2: Description

  - type: textarea
    id: acceptance-criteria
    attributes:
      label: Acceptance Criteria
      description: What would make this feature complete?
      placeholder: |
        - [ ] Criterion 1
        - [ ] Criterion 2
        - [ ] Criterion 3

  - type: textarea
    id: implementation-notes
    attributes:
      label: Implementation Notes
      description: Any technical considerations or implementation suggestions
      placeholder: Add any technical notes here...

  - type: textarea
    id: additional-context
    attributes:
      label: Additional Context
      description: Any other context about the feature request
      placeholder: Add any other context here...

  - type: checkboxes
    id: terms
    attributes:
      label: Confirmation
      description: Please confirm the following
      options:
        - label: I have searched existing issues for similar features
          required: true
        - label: I have read the project documentation
          required: true
        - label: This feature aligns with QuantumOS goals and architecture
          required: true
