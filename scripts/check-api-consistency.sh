#!/bin/bash
#
# QuantumOS API Consistency Checker
#
# Validates that function calls in .c files match declarations in .h files.
# This prevents "phantom API" drift where code calls non-existent functions.
#
# Usage: ./scripts/check-api-consistency.sh [--verbose] [--fix-suggestions]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_DIR="$PROJECT_ROOT/kernel"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

VERBOSE=0
FIX_SUGGESTIONS=0
ERRORS_FOUND=0
WARNINGS_FOUND=0

# Parse arguments
for arg in "$@"; do
    case $arg in
        --verbose|-v)
            VERBOSE=1
            ;;
        --fix-suggestions|-f)
            FIX_SUGGESTIONS=1
            ;;
        --help|-h)
            echo "Usage: $0 [--verbose] [--fix-suggestions]"
            echo ""
            echo "Options:"
            echo "  --verbose, -v         Show detailed output"
            echo "  --fix-suggestions, -f Show suggested fixes for errors"
            echo "  --help, -h            Show this help"
            exit 0
            ;;
    esac
done

echo -e "${BLUE}=== QuantumOS API Consistency Checker ===${NC}"
echo ""

# Build list of declared functions from headers
declare -A DECLARED_FUNCTIONS
declare -A FUNCTION_HEADERS

echo -e "${BLUE}[1/4] Scanning header files for function declarations...${NC}"

for header in $(find "$KERNEL_DIR/include" -name "*.h" 2>/dev/null); do
    header_name=$(basename "$header")

    # Extract function declarations (simplified pattern matching)
    # Matches: return_type function_name(params);
    while IFS= read -r line; do
        # Skip comments and preprocessor
        [[ "$line" =~ ^[[:space:]]*/\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" =~ ^[[:space:]]*// ]] && continue

        # Match function declarations
        if [[ "$line" =~ ^[a-zA-Z_][a-zA-Z0-9_*[:space:]]+[[:space:]]+([a-zA-Z_][a-zA-Z0-9_]*)[[:space:]]*\( ]]; then
            func_name="${BASH_REMATCH[1]}"
            # Skip common non-functions
            [[ "$func_name" == "if" ]] && continue
            [[ "$func_name" == "while" ]] && continue
            [[ "$func_name" == "for" ]] && continue
            [[ "$func_name" == "switch" ]] && continue
            [[ "$func_name" == "return" ]] && continue
            [[ "$func_name" == "sizeof" ]] && continue
            [[ "$func_name" == "typedef" ]] && continue

            DECLARED_FUNCTIONS["$func_name"]=1
            FUNCTION_HEADERS["$func_name"]="$header_name"

            if [ $VERBOSE -eq 1 ]; then
                echo "  Found: $func_name in $header_name"
            fi
        fi
    done < "$header"
done

echo -e "  Found ${GREEN}${#DECLARED_FUNCTIONS[@]}${NC} function declarations"
echo ""

# Scan source files for function calls
echo -e "${BLUE}[2/4] Scanning source files for function calls...${NC}"

declare -A CALLED_FUNCTIONS
declare -A CALL_LOCATIONS

for source in $(find "$KERNEL_DIR/src" -name "*.c" 2>/dev/null); do
    source_name=$(basename "$source")
    line_num=0

    while IFS= read -r line; do
        ((line_num++))

        # Skip comments
        [[ "$line" =~ ^[[:space:]]*/\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*\* ]] && continue
        [[ "$line" =~ ^[[:space:]]*// ]] && continue

        # Find function calls: identifier followed by (
        # This is a simplified check - production would use proper parsing
        while [[ "$line" =~ ([a-zA-Z_][a-zA-Z0-9_]*)[[:space:]]*\( ]]; do
            func_name="${BASH_REMATCH[1]}"

            # Skip common non-functions and keywords
            case "$func_name" in
                if|while|for|switch|return|sizeof|typedef|struct|enum|union|static|extern|const|volatile|inline|__attribute__|ALIGNED|PACKED)
                    ;;
                *)
                    CALLED_FUNCTIONS["$func_name"]=1
                    if [ -z "${CALL_LOCATIONS[$func_name]}" ]; then
                        CALL_LOCATIONS["$func_name"]="$source_name:$line_num"
                    fi
                    ;;
            esac

            # Remove the match to find more in the same line
            line="${line/${BASH_REMATCH[0]}/}"
        done
    done < "$source"
done

echo -e "  Found ${GREEN}${#CALLED_FUNCTIONS[@]}${NC} unique function calls"
echo ""

# Check for undeclared functions (potential phantom APIs)
echo -e "${BLUE}[3/4] Checking for undeclared function calls...${NC}"

# Known external/builtin functions to ignore
KNOWN_EXTERNALS="memset memcpy strlen strncpy printf snprintf boot_log boot_panic early_console_write early_console_init"

for func in "${!CALLED_FUNCTIONS[@]}"; do
    # Skip if declared
    [ "${DECLARED_FUNCTIONS[$func]}" == "1" ] && continue

    # Skip known externals
    skip=0
    for ext in $KNOWN_EXTERNALS; do
        [ "$func" == "$ext" ] && skip=1 && break
    done
    [ $skip -eq 1 ] && continue

    # Skip if it looks like a local/static function (defined in same file)
    # This is a heuristic - might have false positives
    location="${CALL_LOCATIONS[$func]}"
    source_file="${location%%:*}"

    # Check if function is defined in the same source file
    if [ -n "$source_file" ]; then
        full_path="$KERNEL_DIR/src/$source_file"
        if [ -f "$full_path" ]; then
            # Look for function definition
            if grep -q "^[a-zA-Z_][a-zA-Z0-9_* ]*${func}[[:space:]]*(" "$full_path" 2>/dev/null; then
                continue
            fi
        fi
        # Also check ipc subdirectory
        full_path="$KERNEL_DIR/src/ipc/$source_file"
        if [ -f "$full_path" ]; then
            if grep -q "^[a-zA-Z_][a-zA-Z0-9_* ]*${func}[[:space:]]*(" "$full_path" 2>/dev/null; then
                continue
            fi
        fi
    fi

    # This is a potential phantom API call
    echo -e "${RED}ERROR: Undeclared function call: ${func}${NC}"
    echo -e "  Location: ${CALL_LOCATIONS[$func]}"

    if [ $FIX_SUGGESTIONS -eq 1 ]; then
        # Try to find similar function names
        echo -e "  ${YELLOW}Possible matches:${NC}"
        for declared in "${!DECLARED_FUNCTIONS[@]}"; do
            # Simple similarity: same prefix
            prefix="${func:0:4}"
            if [[ "$declared" == *"$prefix"* ]]; then
                echo -e "    - $declared (from ${FUNCTION_HEADERS[$declared]})"
            fi
        done
    fi

    ((ERRORS_FOUND++))
    echo ""
done

if [ $ERRORS_FOUND -eq 0 ]; then
    echo -e "  ${GREEN}No undeclared function calls found${NC}"
fi
echo ""

# Check for common drift patterns
echo -e "${BLUE}[4/4] Checking for known drift patterns...${NC}"

# Pattern 1: Old IPC API that was replaced
OLD_IPC_PATTERNS="ipc_create_queue ipc_destroy_queue ipc_queue_create ipc_queue_destroy"
for pattern in $OLD_IPC_PATTERNS; do
    matches=$(grep -rn "$pattern" "$KERNEL_DIR/src" 2>/dev/null || true)
    if [ -n "$matches" ]; then
        echo -e "${RED}ERROR: Found deprecated IPC API usage: $pattern${NC}"
        echo "$matches" | while read -r match; do
            echo -e "  $match"
        done
        echo -e "  ${YELLOW}Use ipc_process_init() / ipc_process_cleanup() instead${NC}"
        ((ERRORS_FOUND++))
        echo ""
    fi
done

# Pattern 2: Check docs match code
DOC_FILES=$(find "$PROJECT_ROOT/docs" -name "*.md" 2>/dev/null || true)
for doc in $DOC_FILES; do
    for pattern in $OLD_IPC_PATTERNS; do
        if grep -q "$pattern" "$doc" 2>/dev/null; then
            echo -e "${YELLOW}WARNING: Documentation contains outdated API: $pattern${NC}"
            echo -e "  File: $doc"
            ((WARNINGS_FOUND++))
        fi
    done
done

echo ""
echo -e "${BLUE}=== Summary ===${NC}"
if [ $ERRORS_FOUND -eq 0 ] && [ $WARNINGS_FOUND -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC}"
    exit 0
else
    if [ $ERRORS_FOUND -gt 0 ]; then
        echo -e "${RED}Errors: $ERRORS_FOUND${NC}"
    fi
    if [ $WARNINGS_FOUND -gt 0 ]; then
        echo -e "${YELLOW}Warnings: $WARNINGS_FOUND${NC}"
    fi
    echo ""
    echo "Please fix the errors before submitting a PR."
    exit 1
fi
