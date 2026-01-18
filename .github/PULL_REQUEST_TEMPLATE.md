# QuantumOS Pull Request Template

## Description
<!-- Brief description of changes made -->

## Type of Change
<!-- Check all that apply -->
- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update
- [ ] Performance improvement
- [ ] Security enhancement
- [ ] Quantum-specific feature

## Issue Reference
<!-- Reference any related issues (e.g., "Fixes #123") -->
Fixes #

## Testing
<!-- Describe how you tested your changes -->
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Manual testing completed
- [ ] Performance benchmarks run
- [ ] Security tests passed

### Test Details
<!-- Add specific test information -->
```bash
# Commands run for testing
make test
make run
```

## Checklist
<!-- Review the checklist for completeness -->
- [ ] My code follows the project's coding standards
- [ ] I have performed a self-review of my own code
- [ ] I have commented my code, particularly in hard-to-understand areas
- [ ] I have made corresponding changes to the documentation
- [ ] My changes generate no new warnings
- [ ] I have added tests that prove my fix is effective or that my feature works
- [ ] New and existing unit tests pass locally with my changes
- [ ] Any dependent changes have been merged and published in downstream modules

## Performance Impact
<!-- Describe any performance impact -->
- [ ] No performance impact
- [ ] Performance improvement (describe)
- [ ] Performance regression (explain why acceptable)

### Performance Metrics
<!-- Add specific performance data -->
- Before: [metric]
- After: [metric]
- Improvement: [percentage]

## Security Considerations
<!-- Address any security implications -->
- [ ] No security impact
- [ ] Security enhancement (describe)
- [ ] Security considerations addressed (explain)

### Security Review
- [ ] Reviewed for common vulnerabilities
- [ ] Input validation implemented
- [ ] Capability-based security enforced
- [ ] No privilege escalation vectors

## Quantum-Specific Changes
<!-- For quantum-related changes -->
- [ ] No quantum impact
- [ ] Quantum feature added (describe)
- [ ] Quantum performance improvement (describe)
- [ ] Quantum security enhancement (describe)

### Quantum Testing
- [ ] Tested with quantum simulator
- [ ] Tested with quantum hardware (if available)
- [ ] Coherence time validation completed
- [ ] Quantum error handling tested

## Breaking Changes
<!-- List any breaking changes -->
- [ ] No breaking changes
- [ ] Breaking changes (list and explain):

### Migration Guide
<!-- If breaking changes, provide migration steps -->
1. Step 1
2. Step 2
3. Step 3

## Documentation
<!-- Documentation changes -->
- [ ] No documentation changes needed
- [ ] Documentation updated (list files)
- [ ] New documentation added (list files)

### Documentation Files Changed
- [ ] README.md
- [ ] API documentation
- [ ] Architecture docs
- [ ] User guides
- [ ] Developer guides

## Additional Context
<!-- Add any other context about the change here -->
<!-- Include screenshots, diagrams, or other visual aids if helpful -->

## Review Focus Areas
<!-- What should reviewers focus on? -->
- [ ] Code correctness
- [ ] Performance impact
- [ ] Security implications
- [ ] Quantum-specific considerations
- [ ] Documentation accuracy
- [ ] Test coverage

## Testing Environment
<!-- Describe your testing environment -->
- OS: [e.g., Ubuntu 22.04]
- Architecture: [e.g., x86_64]
- Compiler: [e.g., gcc-x86_64-elf 11.2]
- QEMU version: [e.g., 7.0]
- Hardware: [if applicable]

## Verification Steps
<!-- Steps for reviewers to verify the changes -->
1. Build the kernel: `make`
2. Run in QEMU: `make run`
3. Verify [specific behavior]
4. Check [performance metric]
5. Test [quantum feature]

## Screenshots/Videos
<!-- Add screenshots or videos if applicable -->
<!-- Upload images to the PR and link them here -->

## Additional Notes
<!-- Any additional information reviewers should know -->

---

## Reviewer Checklist
<!-- For reviewers to complete -->
- [ ] Code is well-written and follows standards
- [ ] Tests are comprehensive and pass
- [ ] Documentation is accurate and complete
- [ ] Performance impact is acceptable
- [ ] Security implications are addressed
- [ ] Quantum-specific aspects are correct
- [ ] Breaking changes are justified and documented

## Approval Requirements
<!-- Minimum requirements for approval -->
- [ ] At least one maintainer approval
- [ ] All automated checks pass
- [ ] No outstanding security concerns
- [ ] Documentation is up to date

Thank you for contributing to QuantumOS! 🚀
