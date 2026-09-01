#!/bin/bash
set -euo pipefail

# osc-build.sh — Monitor and interact with OBS builds for Plan-V4D
#
# Usage:
#   ./osc-build.sh                          # monitor both builds
#   ./osc-build.sh --status                 # quick status check
#   ./osc-build.sh --results                # download built RPMs
#   ./osc-build.sh --rebuild openSUSE       # force rebuild for one target
#   ./osc-build.sh --submit openSUSE_Tumbleweed  # submit to devel project

OBS_API="${OBS_API:-https://api.opensuse.org}"
OBS_USER="${OSC_USER:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- Determine OBS username ----
if [[ -z "$OBS_USER" ]]; then
    if [[ -f ~/.oscrc ]]; then
        OBS_USER=$(grep -m1 '^\s*user\s*=' ~/.oscrc | sed 's/.*=\s*//')
    fi
    if [[ -z "$OBS_USER" ]]; then
        echo "ERROR: Could not determine OBS username." >&2
        exit 1
    fi
fi

TOP_PROJECT="home:${OBS_USER}"
PACKAGE="plan-v4d"
PROJECTS=("${TOP_PROJECT}:Plan-V4D:openSUSE_Tumbleweed" "${TOP_PROJECT}:Plan-V4D:Fedora")

ACTION="${1:-monitor}"

# ---- Helper: short repo name from full project path ----
repo_name() {
    local proj="$1"
    echo "${proj##*:}"
}

# ---- Monitor builds ----
do_monitor() {
    local status_only="${1:-false}"

    for proj in "${PROJECTS[@]}"; do
        local short
        short=$(repo_name "$proj")
        echo "=== ${short} (${proj}) ==="

        if [[ "$status_only" == "true" ]]; then
            osc -A "$OBS_API" results -V "$proj/$PACKAGE" 2>/dev/null || echo "  (no results yet)"
        else
            osc -A "$OBS_API" results -V "$proj/$PACKAGE" 2>/dev/null || echo "  (no results yet)"
            echo ""
            # Show last few build logs
            osc -A "$OBS_API" lifeguard "$proj/$PACKAGE" 2>/dev/null || true
        fi
        echo ""
    done
}

# ---- Download built RPMs ----
do_results() {
    for proj in "${PROJECTS[@]}"; do
        local short
        short=$(repo_name "$proj")
        local outdir="${SCRIPT_DIR}/results/${short}"
        mkdir -p "$outdir"

        echo "=== Downloading RPMs for ${short} ==="
        osc -A "$OBS_API" getbinaries "$proj/$PACKAGE" "$outdir" 2>/dev/null || echo "  (no binaries yet)"
        echo "  Output: $outdir"
        echo ""
    done
}

# ---- Force rebuild ----
do_rebuild() {
    local target="${1:-}"
    if [[ -z "$target" ]]; then
        echo "Usage: $0 --rebuild <openSUSE_Tumbleweed|Fedora>" >&2
        exit 1
    fi

    for proj in "${PROJECTS[@]}"; do
        if [[ "$proj" == *":$target" ]]; then
            echo "=== Triggering rebuild: ${proj}/${PACKAGE} ==="
            osc -A "$OBS_API" rebuild "$proj/$PACKAGE" "$target" 2>/dev/null
            echo "  Rebuild triggered."
            return 0
        fi
    done

    echo "ERROR: Target '${target}' not found in project list." >&2
    echo "Available targets:" >&2
    for proj in "${PROJECTS[@]}"; do
        echo "  $(repo_name "$proj")" >&2
    done
    exit 1
}

# ---- Submit to OBS devel project (openSUSE:Factory or Fedora) ----
do_submit() {
    local target="${1:-}"
    local obs_target_project=""

    case "$target" in
        openSUSE*)
            obs_target_project="openSUSE:Factory"
            ;;
        Fedora*)
            obs_target_project="Fedora:Rawhide"
            ;;
        *)
            echo "Usage: $0 --submit <openSUSE_Tumbleweed|Fedora>" >&2
            exit 1
            ;;
    esac

    for proj in "${PROJECTS[@]}"; do
        if [[ "$proj" == *":$target" ]]; then
            echo "=== Submitting ${proj}/${PACKAGE} -> ${obs_target_project} ==="
            osc -A "$OBS_API" sr -m "Submit ${PACKAGE} ${target}" \
                "$proj/$PACKAGE" "$obs_target_project/$PACKAGE" 2>/dev/null
            echo "  Submission request created."
            return 0
        fi
    done

    echo "ERROR: Target '${target}' not found." >&2
    exit 1
}

# ---- Main ----
case "$ACTION" in
    --status)
        do_monitor true
        ;;
    --results)
        do_results
        ;;
    --rebuild)
        do_rebuild "${2:-}"
        ;;
    --submit)
        do_submit "${2:-}"
        ;;
    monitor|"")
        do_monitor false
        ;;
    *)
        echo "Usage: $0 [--status|--results|--rebuild <target>|--submit <target>]" >&2
        echo ""
        echo "Targets: openSUSE_Tumbleweed, Fedora" >&2
        exit 1
        ;;
esac
