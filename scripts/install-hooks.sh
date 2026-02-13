#!/bin/bash
#
# Install QuantumOS Git Hooks
#
# Usage: ./scripts/install-hooks.sh
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
HOOKS_SRC="$PROJECT_ROOT/.github/hooks"
HOOKS_DST="$PROJECT_ROOT/.git/hooks"

echo "Installing QuantumOS git hooks..."

# Check if we're in a git repo
if [ ! -d "$PROJECT_ROOT/.git" ]; then
    echo "Error: Not a git repository"
    exit 1
fi

# Create hooks directory if needed
mkdir -p "$HOOKS_DST"

# Install pre-commit hook
if [ -f "$HOOKS_SRC/pre-commit" ]; then
    cp "$HOOKS_SRC/pre-commit" "$HOOKS_DST/pre-commit"
    chmod +x "$HOOKS_DST/pre-commit"
    echo "  Installed: pre-commit"
fi

# Make validation scripts executable
chmod +x "$SCRIPT_DIR"/*.sh 2>/dev/null || true

echo ""
echo "Git hooks installed successfully!"
echo ""
echo "Hooks will run automatically on:"
echo "  - pre-commit: Validates code before commit"
echo ""
echo "To bypass hooks (not recommended):"
echo "  git commit --no-verify"
