#!/bin/bash
#
# QuantumOS Contribution Validator
#
# Comprehensive pre-PR validation for AI and human contributors.
# Run this before submitting any pull request.
#
# Usage: ./scripts/validate-contribution.sh [--quick]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

QUICK_MODE=0
TOTAL_CHECKS=0
PASSED_CHECKS=0
FAILED_CHECKS=0

for arg in "$@"; do
    case $arg in
        --quick|-q)
            QUICK_MODE=1
            ;;
    esac
done

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED_CHECKS++))
    ((TOTAL_CHECKS++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED_CHECKS++))
    ((TOTAL_CHECKS++))
}

log_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((TOTAL_CHECKS++))
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

echo ""
echo -e "${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║          QuantumOS Contribution Validator                     ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check 1: API Consistency
echo -e "${BLUE}[1/6] API Consistency Check${NC}"
if [ -f "$SCRIPT_DIR/check-api-consistency.sh" ]; then
    if bash "$SCRIPT_DIR/check-api-consistency.sh" > /tmp/api-check.log 2>&1; then
        log_pass "API consistency verified"
    else
        log_fail "API consistency check failed"
        echo "  Details in /tmp/api-check.log"
        cat /tmp/api-check.log | head -20
    fi
else
    log_skip "API consistency script not found"
fi
echo ""

# Check 2: Compilation
echo -e "${BLUE}[2/6] Compilation Check${NC}"
cd "$PROJECT_ROOT"
if make clean > /dev/null 2>&1; then
    if make > /tmp/build.log 2>&1; then
        log_pass "Code compiles successfully"
    else
        log_fail "Compilation failed"
        echo "  Build errors:"
        tail -20 /tmp/build.log
    fi
else
    log_skip "Make clean failed (may be first build)"
    if make > /tmp/build.log 2>&1; then
        log_pass "Code compiles successfully"
    else
        log_fail "Compilation failed"
    fi
fi
echo ""

# Check 3: Header Guards
echo -e "${BLUE}[3/6] Header Guard Check${NC}"
MISSING_GUARDS=0
for header in $(find "$PROJECT_ROOT/kernel/include" -name "*.h" 2>/dev/null); do
    header_name=$(basename "$header" .h | tr '[:lower:]' '[:upper:]')
    if ! grep -q "#ifndef.*${header_name}" "$header" 2>/dev/null; then
        if ! grep -q "#ifndef" "$header" 2>/dev/null; then
            echo "  Missing guard: $header"
            ((MISSING_GUARDS++))
        fi
    fi
done
if [ $MISSING_GUARDS -eq 0 ]; then
    log_pass "All headers have proper guards"
else
    log_fail "Found $MISSING_GUARDS headers without guards"
fi
echo ""

# Check 4: Documentation Sync (quick check)
echo -e "${BLUE}[4/6] Documentation Sync Check${NC}"
DOC_ISSUES=0

# Check for old API patterns in docs
OLD_PATTERNS="ipc_create_queue ipc_destroy_queue"
for pattern in $OLD_PATTERNS; do
    if grep -r "$pattern" "$PROJECT_ROOT/docs" 2>/dev/null | grep -v "\.git"; then
        echo "  Found outdated pattern in docs: $pattern"
        ((DOC_ISSUES++))
    fi
done

if [ $DOC_ISSUES -eq 0 ]; then
    log_pass "Documentation appears in sync"
else
    log_fail "Documentation contains outdated patterns"
fi
echo ""

# Check 5: Commit Message Format (if in git repo)
echo -e "${BLUE}[5/6] Commit Message Check${NC}"
if git rev-parse --git-dir > /dev/null 2>&1; then
    LAST_COMMIT=$(git log -1 --pretty=format:"%s" 2>/dev/null || echo "")
    if [ -n "$LAST_COMMIT" ]; then
        # Check for conventional commit format
        if [[ "$LAST_COMMIT" =~ ^(feat|fix|docs|style|refactor|test|chore)\(.+\):.+ ]]; then
            log_pass "Commit message follows convention"
        else
            log_fail "Commit message doesn't follow conventional format"
            echo "  Expected: type(scope): description"
            echo "  Got: $LAST_COMMIT"
        fi
    else
        log_skip "No commits to check"
    fi
else
    log_skip "Not in a git repository"
fi
echo ""

# Check 6: AI Contributor Metadata (optional but recommended)
echo -e "${BLUE}[6/6] AI Contributor Metadata Check${NC}"
if git rev-parse --git-dir > /dev/null 2>&1; then
    LAST_BODY=$(git log -1 --pretty=format:"%b" 2>/dev/null || echo "")
    if [[ "$LAST_BODY" =~ AI-Contributor: ]]; then
        log_pass "AI contributor metadata present"
        # Extract and display
        echo "$LAST_BODY" | grep -E "^AI-" | while read -r line; do
            echo "  $line"
        done
    else
        log_info "No AI contributor metadata (optional for human contributors)"
        ((TOTAL_CHECKS++))
    fi
else
    log_skip "Not in a git repository"
fi
echo ""

# Summary
echo -e "${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║                        Summary                                ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Total checks:  $TOTAL_CHECKS"
echo -e "  ${GREEN}Passed:        $PASSED_CHECKS${NC}"
echo -e "  ${RED}Failed:        $FAILED_CHECKS${NC}"
echo ""

if [ $FAILED_CHECKS -eq 0 ]; then
    echo -e "${GREEN}All validations passed! Ready for PR.${NC}"
    exit 0
else
    echo -e "${RED}Please fix the failed checks before submitting a PR.${NC}"
    exit 1
fi
