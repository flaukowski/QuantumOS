#!/bin/bash
#
# QuantumOS Contributor Tracker
#
# Tracks AI and human contributions with quality metrics.
# Helps identify drift patterns and contributor reliability.
#
# Usage:
#   ./scripts/track-contributor.sh log          # Log current commit
#   ./scripts/track-contributor.sh report       # Generate report
#   ./scripts/track-contributor.sh stats [name] # Show stats for contributor
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TRACKER_FILE="$PROJECT_ROOT/.github/contributor-metrics.json"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Initialize tracker file if it doesn't exist
init_tracker() {
    if [ ! -f "$TRACKER_FILE" ]; then
        mkdir -p "$(dirname "$TRACKER_FILE")"
        cat > "$TRACKER_FILE" << 'EOF'
{
  "version": "1.0",
  "description": "QuantumOS AI Contributor Metrics",
  "contributors": {},
  "contributions": [],
  "quality_events": []
}
EOF
        echo "Initialized contributor tracker at $TRACKER_FILE"
    fi
}

# Log a contribution from current commit
log_contribution() {
    init_tracker

    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        echo "Error: Not in a git repository"
        exit 1
    fi

    COMMIT_HASH=$(git log -1 --pretty=format:"%H")
    COMMIT_SHORT=$(git log -1 --pretty=format:"%h")
    COMMIT_MSG=$(git log -1 --pretty=format:"%s")
    COMMIT_BODY=$(git log -1 --pretty=format:"%b")
    COMMIT_DATE=$(git log -1 --pretty=format:"%ci")
    AUTHOR_NAME=$(git log -1 --pretty=format:"%an")
    AUTHOR_EMAIL=$(git log -1 --pretty=format:"%ae")

    # Extract AI metadata if present
    AI_CONTRIBUTOR=""
    AI_CONFIDENCE=""
    AI_VERIFIED=""

    if [[ "$COMMIT_BODY" =~ AI-Contributor:[[:space:]]*([^[:space:]]+) ]]; then
        AI_CONTRIBUTOR="${BASH_REMATCH[1]}"
    fi
    if [[ "$COMMIT_BODY" =~ AI-Confidence:[[:space:]]*([^[:space:]]+) ]]; then
        AI_CONFIDENCE="${BASH_REMATCH[1]}"
    fi
    if [[ "$COMMIT_BODY" =~ AI-Verified:[[:space:]]*([^[:space:]]+) ]]; then
        AI_VERIFIED="${BASH_REMATCH[1]}"
    fi

    # Get files changed
    FILES_CHANGED=$(git diff-tree --no-commit-id --name-only -r "$COMMIT_HASH" | wc -l)
    LINES_ADDED=$(git log -1 --pretty=format: --numstat | awk '{sum += $1} END {print sum}')
    LINES_REMOVED=$(git log -1 --pretty=format: --numstat | awk '{sum += $2} END {print sum}')

    # Determine contributor type
    CONTRIBUTOR_TYPE="human"
    CONTRIBUTOR_ID="$AUTHOR_NAME"
    if [ -n "$AI_CONTRIBUTOR" ]; then
        CONTRIBUTOR_TYPE="ai"
        CONTRIBUTOR_ID="$AI_CONTRIBUTOR"
    elif [[ "$AUTHOR_EMAIL" =~ noreply@anthropic\.com ]]; then
        CONTRIBUTOR_TYPE="ai"
        CONTRIBUTOR_ID="Claude"
    fi

    echo -e "${BLUE}Logging contribution:${NC}"
    echo "  Commit: $COMMIT_SHORT"
    echo "  Message: $COMMIT_MSG"
    echo "  Contributor: $CONTRIBUTOR_ID ($CONTRIBUTOR_TYPE)"
    echo "  Files: $FILES_CHANGED, +$LINES_ADDED/-$LINES_REMOVED"

    if [ -n "$AI_CONFIDENCE" ]; then
        echo "  AI Confidence: $AI_CONFIDENCE"
    fi
    if [ -n "$AI_VERIFIED" ]; then
        echo "  AI Verified: $AI_VERIFIED"
    fi

    # Create contribution entry (simplified - in production would use jq)
    TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

    # Append to a simple log file as well
    LOG_FILE="$PROJECT_ROOT/.github/contributions.log"
    echo "$TIMESTAMP|$COMMIT_SHORT|$CONTRIBUTOR_ID|$CONTRIBUTOR_TYPE|$COMMIT_MSG|$FILES_CHANGED|$LINES_ADDED|$LINES_REMOVED|$AI_CONFIDENCE|$AI_VERIFIED" >> "$LOG_FILE"

    echo -e "${GREEN}Contribution logged successfully${NC}"
}

# Log a quality event (review iteration, fix needed, etc.)
log_quality_event() {
    init_tracker

    EVENT_TYPE="$1"
    CONTRIBUTOR="$2"
    DESCRIPTION="$3"

    if [ -z "$EVENT_TYPE" ] || [ -z "$CONTRIBUTOR" ]; then
        echo "Usage: $0 quality <event_type> <contributor> [description]"
        echo "Event types: review_iteration, api_error, compile_error, doc_mismatch, fix_applied"
        exit 1
    fi

    TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    LOG_FILE="$PROJECT_ROOT/.github/quality-events.log"

    echo "$TIMESTAMP|$EVENT_TYPE|$CONTRIBUTOR|$DESCRIPTION" >> "$LOG_FILE"

    echo -e "${GREEN}Quality event logged: $EVENT_TYPE for $CONTRIBUTOR${NC}"
}

# Generate contributor report
generate_report() {
    init_tracker

    LOG_FILE="$PROJECT_ROOT/.github/contributions.log"
    QUALITY_FILE="$PROJECT_ROOT/.github/quality-events.log"

    echo ""
    echo -e "${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║            QuantumOS Contributor Report                       ║${NC}"
    echo -e "${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""

    if [ ! -f "$LOG_FILE" ]; then
        echo "No contributions logged yet."
        echo "Run: $0 log  (after making a commit)"
        return
    fi

    # Count contributions by contributor
    echo -e "${BLUE}Contributions by Contributor:${NC}"
    echo ""
    printf "  %-25s %-10s %-10s %-10s\n" "Contributor" "Type" "Commits" "Lines"
    printf "  %-25s %-10s %-10s %-10s\n" "-------------------------" "----------" "----------" "----------"

    # Process log file
    declare -A CONTRIB_COMMITS
    declare -A CONTRIB_LINES
    declare -A CONTRIB_TYPE

    while IFS='|' read -r timestamp commit contributor type msg files added removed confidence verified; do
        CONTRIB_COMMITS["$contributor"]=$((${CONTRIB_COMMITS["$contributor"]:-0} + 1))
        CONTRIB_LINES["$contributor"]=$((${CONTRIB_LINES["$contributor"]:-0} + ${added:-0}))
        CONTRIB_TYPE["$contributor"]="$type"
    done < "$LOG_FILE"

    for contrib in "${!CONTRIB_COMMITS[@]}"; do
        printf "  %-25s %-10s %-10s %-10s\n" "$contrib" "${CONTRIB_TYPE[$contrib]}" "${CONTRIB_COMMITS[$contrib]}" "+${CONTRIB_LINES[$contrib]}"
    done

    echo ""

    # Quality metrics
    if [ -f "$QUALITY_FILE" ]; then
        echo -e "${BLUE}Quality Events:${NC}"
        echo ""

        declare -A QUALITY_COUNTS

        while IFS='|' read -r timestamp event contributor description; do
            key="${contributor}|${event}"
            QUALITY_COUNTS["$key"]=$((${QUALITY_COUNTS["$key"]:-0} + 1))
        done < "$QUALITY_FILE"

        printf "  %-25s %-20s %-10s\n" "Contributor" "Event Type" "Count"
        printf "  %-25s %-20s %-10s\n" "-------------------------" "--------------------" "----------"

        for key in "${!QUALITY_COUNTS[@]}"; do
            IFS='|' read -r contrib event <<< "$key"
            printf "  %-25s %-20s %-10s\n" "$contrib" "$event" "${QUALITY_COUNTS[$key]}"
        done

        echo ""
    fi

    # AI-specific metrics
    echo -e "${BLUE}AI Contributor Summary:${NC}"
    echo ""

    AI_COUNT=0
    HUMAN_COUNT=0

    for contrib in "${!CONTRIB_TYPE[@]}"; do
        if [ "${CONTRIB_TYPE[$contrib]}" == "ai" ]; then
            ((AI_COUNT++))
        else
            ((HUMAN_COUNT++))
        fi
    done

    echo "  AI Contributors: $AI_COUNT"
    echo "  Human Contributors: $HUMAN_COUNT"
    echo ""

    # Recent contributions
    echo -e "${BLUE}Recent Contributions (last 10):${NC}"
    echo ""
    tail -10 "$LOG_FILE" | while IFS='|' read -r timestamp commit contributor type msg files added removed confidence verified; do
        printf "  %s %-10s %-20s %s\n" "$commit" "($type)" "$contributor" "$msg"
    done
    echo ""
}

# Show stats for specific contributor
show_stats() {
    CONTRIBUTOR="$1"

    if [ -z "$CONTRIBUTOR" ]; then
        echo "Usage: $0 stats <contributor_name>"
        exit 1
    fi

    LOG_FILE="$PROJECT_ROOT/.github/contributions.log"
    QUALITY_FILE="$PROJECT_ROOT/.github/quality-events.log"

    if [ ! -f "$LOG_FILE" ]; then
        echo "No contributions logged yet."
        return
    fi

    echo ""
    echo -e "${BLUE}Stats for: $CONTRIBUTOR${NC}"
    echo ""

    COMMITS=0
    LINES=0
    FILES=0

    while IFS='|' read -r timestamp commit contrib type msg file_count added removed confidence verified; do
        if [ "$contrib" == "$CONTRIBUTOR" ]; then
            ((COMMITS++))
            LINES=$((LINES + ${added:-0}))
            FILES=$((FILES + ${file_count:-0}))
        fi
    done < "$LOG_FILE"

    echo "  Total commits: $COMMITS"
    echo "  Total lines added: $LINES"
    echo "  Total files touched: $FILES"

    if [ -f "$QUALITY_FILE" ]; then
        echo ""
        echo "  Quality events:"
        grep "|$CONTRIBUTOR|" "$QUALITY_FILE" 2>/dev/null | while IFS='|' read -r timestamp event contrib description; do
            echo "    - $event: $description"
        done
    fi

    echo ""
}

# Main command router
case "${1:-}" in
    log)
        log_contribution
        ;;
    quality)
        log_quality_event "$2" "$3" "$4"
        ;;
    report)
        generate_report
        ;;
    stats)
        show_stats "$2"
        ;;
    init)
        init_tracker
        ;;
    *)
        echo "QuantumOS Contributor Tracker"
        echo ""
        echo "Usage: $0 <command> [args]"
        echo ""
        echo "Commands:"
        echo "  log                  Log current commit as a contribution"
        echo "  quality <type> <contributor> [desc]  Log a quality event"
        echo "  report               Generate contributor report"
        echo "  stats <name>         Show stats for specific contributor"
        echo "  init                 Initialize tracker files"
        echo ""
        echo "Quality event types:"
        echo "  review_iteration, api_error, compile_error, doc_mismatch, fix_applied"
        ;;
esac
