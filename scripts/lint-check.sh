#!/bin/bash
#
# QuantumOS Lint Checker
#
# Quick checks for common code quality issues.
#
# Usage: ./scripts/lint-check.sh [--fix]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_DIR="$PROJECT_ROOT/kernel"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

ISSUES_FOUND=0

echo -e "${BLUE}=== QuantumOS Lint Checker ===${NC}"
echo ""

# Check 1: Trailing whitespace
echo -e "${BLUE}[1/5] Checking for trailing whitespace...${NC}"
TRAILING=$(find "$KERNEL_DIR" -name "*.c" -o -name "*.h" | xargs grep -l "[[:space:]]$" 2>/dev/null || true)
if [ -n "$TRAILING" ]; then
    echo -e "${YELLOW}WARNING: Files with trailing whitespace:${NC}"
    echo "$TRAILING" | head -5
    ((ISSUES_FOUND++))
else
    echo -e "${GREEN}No trailing whitespace found${NC}"
fi
echo ""

# Check 2: Missing newline at end of file
echo -e "${BLUE}[2/5] Checking for missing final newline...${NC}"
MISSING_NL=0
for file in $(find "$KERNEL_DIR" -name "*.c" -o -name "*.h" 2>/dev/null); do
    if [ -s "$file" ] && [ "$(tail -c 1 "$file" | wc -l)" -eq 0 ]; then
        echo "  $file"
        ((MISSING_NL++))
    fi
done
if [ $MISSING_NL -gt 0 ]; then
    echo -e "${YELLOW}WARNING: $MISSING_NL files missing final newline${NC}"
    ((ISSUES_FOUND++))
else
    echo -e "${GREEN}All files have proper final newline${NC}"
fi
echo ""

# Check 3: TODO/FIXME/XXX comments
echo -e "${BLUE}[3/5] Checking for TODO/FIXME comments...${NC}"
TODO_COUNT=$(grep -r "TODO\|FIXME\|XXX" "$KERNEL_DIR" --include="*.c" --include="*.h" 2>/dev/null | wc -l || echo 0)
if [ "$TODO_COUNT" -gt 0 ]; then
    echo -e "${YELLOW}INFO: Found $TODO_COUNT TODO/FIXME comments${NC}"
    grep -rn "TODO\|FIXME" "$KERNEL_DIR" --include="*.c" --include="*.h" 2>/dev/null | head -5
    echo "  ..."
else
    echo -e "${GREEN}No TODO/FIXME comments found${NC}"
fi
echo ""

# Check 4: Long lines (>100 chars)
echo -e "${BLUE}[4/5] Checking for long lines (>100 chars)...${NC}"
LONG_LINES=0
for file in $(find "$KERNEL_DIR" -name "*.c" -o -name "*.h" 2>/dev/null); do
    count=$(awk 'length > 100' "$file" 2>/dev/null | wc -l || echo 0)
    if [ "$count" -gt 0 ]; then
        ((LONG_LINES += count))
    fi
done
if [ $LONG_LINES -gt 0 ]; then
    echo -e "${YELLOW}WARNING: Found $LONG_LINES lines over 100 characters${NC}"
    ((ISSUES_FOUND++))
else
    echo -e "${GREEN}No excessively long lines found${NC}"
fi
echo ""

# Check 5: Deprecated patterns
echo -e "${BLUE}[5/5] Checking for deprecated patterns...${NC}"
DEPRECATED_PATTERNS="ipc_create_queue ipc_destroy_queue gets sprintf strcpy strcat"
DEPRECATED_FOUND=0
for pattern in $DEPRECATED_PATTERNS; do
    if grep -r "$pattern" "$KERNEL_DIR/src" --include="*.c" 2>/dev/null | grep -v "^Binary"; then
        echo -e "${RED}Found deprecated pattern: $pattern${NC}"
        ((DEPRECATED_FOUND++))
    fi
done
if [ $DEPRECATED_FOUND -gt 0 ]; then
    echo -e "${RED}ERROR: Found $DEPRECATED_FOUND deprecated patterns${NC}"
    ((ISSUES_FOUND++))
else
    echo -e "${GREEN}No deprecated patterns found${NC}"
fi
echo ""

# Summary
echo -e "${BLUE}=== Summary ===${NC}"
if [ $ISSUES_FOUND -eq 0 ]; then
    echo -e "${GREEN}All lint checks passed!${NC}"
    exit 0
else
    echo -e "${YELLOW}Found $ISSUES_FOUND issue categories${NC}"
    echo "Consider fixing before PR submission."
    exit 0  # Don't fail on lint warnings, only on errors
fi
