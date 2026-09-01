#!/bin/bash
set -euo pipefail

# osc-setup.sh — Create OBS projects, packages, and upload sources for Plan-V4D
#
# Usage:
#   ./osc-setup.sh                    # uses default OBS_USER from osc config
#   ./osc-setup.sh myobsuser          # explicit OBS username
#   OBS_API=https://api.opensuse.org OSC_USER=me ./osc-setup.sh
#
# Prerequisites:
#   - osc installed (zypper install osc / dnf install osc)
#   - osc configured (run 'osc ls' once to trigger ~/.oscrc setup)

OBS_API="${OBS_API:-https://api.opensuse.org}"
OBS_USER="${1:-${OSC_USER:-}}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OBS_DIR="${SCRIPT_DIR}"

# ---- Read OBS credentials from oscrc if not provided ----
if [[ -z "$OBS_USER" ]]; then
    if [[ -f ~/.oscrc ]]; then
        OBS_USER=$(grep -m1 '^\s*user\s*=' ~/.oscrc | sed 's/.*=\s*//')
    fi
    if [[ -z "$OBS_USER" ]]; then
        echo "ERROR: Could not determine OBS username." >&2
        echo "Usage: $0 <obs-username>" >&2
        echo "   or: OSC_USER=<username> $0" >&2
        exit 1
    fi
fi

echo "=== Plan-V4D OBS Setup ==="
echo "OBS API:   $OBS_API"
echo "OBS User:  $OBS_USER"
echo ""

# ---- Verify osc is available ----
if ! command -v osc &>/dev/null; then
    echo "ERROR: 'osc' not found. Install it:" >&2
    echo "  openSUSE: sudo zypper install obs-service-obs_scm osc" >&2
    echo "  Fedora:   sudo dnf install osc" >&2
    exit 1
fi

# ---- Project names ----
TOP_PROJECT="home:${OBS_USER}"
SUB_PROJECT_TW="${TOP_PROJECT}:Plan-V4D:openSUSE_Tumbleweed"
SUB_PROJECT_FEDORA="${TOP_PROJECT}:Plan-V4D:Fedora"
PACKAGE="plan-v4d"

# ---- Create projects ----
echo "--- Creating top-level project: ${TOP_PROJECT} ---"
osc -A "$OBS_API" meta prj -e "$TOP_PROJECT" --file - 2>/dev/null <<METAEOF || true
<project name="${TOP_PROJECT}">
  <title>Plan-V4D Packages</title>
  <description>OpenCV with Plan-DSL and V4D Visualization Modules (custom build)</description>
  <person userid="${OBS_USER}" role="maintainer"/>
  <repository name="openSUSE_Tumbleweed">
    <path project="openSUSE:Factory" repository="standard"/>
    <arch>x86_64</arch>
  </repository>
  <repository name="Fedora">
    <path project="Fedora:Rawhide" repository="rawhide"/>
    <arch>x86_64</arch>
  </repository>
</project>
METAEOF
echo "  Done."

echo ""
echo "--- Creating openSUSE Tumbleweed sub-project: ${SUB_PROJECT_TW} ---"
osc -A "$OBS_API" meta prj -e "$SUB_PROJECT_TW" --file - 2>/dev/null <<METAEOF || true
<project name="${SUB_PROJECT_TW}">
  <title>Plan-V4D for openSUSE Tumbleweed</title>
  <description>OpenCV+Plan-V4D packages targeting openSUSE Tumbleweed</description>
  <person userid="${OBS_USER}" role="maintainer"/>
  <repository name="openSUSE_Tumbleweed">
    <path project="openSUSE:Factory" repository="standard"/>
    <arch>x86_64</arch>
  </repository>
  <build>
    <enable/>
  </build>
  <publish>
    <enable/>
  </publish>
</project>
METAEOF
echo "  Done."

echo ""
echo "--- Creating Fedora sub-project: ${SUB_PROJECT_FEDORA} ---"
osc -A "$OBS_API" meta prj -e "$SUB_PROJECT_FEDORA" --file - 2>/dev/null <<METAEOF || true
<project name="${SUB_PROJECT_FEDORA}">
  <title>Plan-V4D for Fedora</title>
  <description>OpenCV+Plan-V4D packages targeting Fedora Rawhide/latest</description>
  <person userid="${OBS_USER}" role="maintainer"/>
  <repository name="Fedora">
    <path project="Fedora:Rawhide" repository="rawhide"/>
    <arch>x86_64</arch>
  </repository>
  <build>
    <enable/>
  </build>
  <publish>
    <enable/>
  </publish>
</project>
METAEOF
echo "  Done."

# ---- Create packages and upload sources ----
for PROJECT in "$SUB_PROJECT_TW" "$SUB_PROJECT_FEDORA"; do
    echo ""
    echo "--- Setting up package in ${PROJECT} ---"

    PKG_DIR="/tmp/obs-${PACKAGE}-$$/$(echo "$PROJECT" | tr ':' '_')"
    mkdir -p "$PKG_DIR"

    # Copy spec file
    cp "${OBS_DIR}/plan-v4d.spec" "$PKG_DIR/"
    cp "${OBS_DIR}/_service"      "$PKG_DIR/"

    # Import into OBS
    osc -A "$OBS_API" checkout "$PROJECT/$PACKAGE" 2>/dev/null || true
    if [[ -d "$PROJECT/$PACKAGE" ]]; then
        cp "$PKG_DIR"/* "$PROJECT/$PACKAGE/" 2>/dev/null || true
    else
        mkdir -p "$PROJECT/$PACKAGE"
        cp "$PKG_DIR"/* "$PROJECT/$PACKAGE/"
    fi

    # Add and commit
    pushd "$PROJECT/$PACKAGE" >/dev/null
    osc -A "$OBS_API" add --force plan-v4d.spec _service 2>/dev/null || true
    osc -A "$OBS_API" commit -m "Initial upload: plan-v4d ${PROJECT##*:}" --no-verify
    popd >/dev/null
    echo "  Committed sources to ${PROJECT}/${PACKAGE}"
done

# ---- Cleanup ----
rm -rf "/tmp/obs-${PACKAGE}-$$"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Monitor builds:"
echo "       ./osc-build.sh"
echo "  2. Or check on web:"
echo "       ${OBS_API}/${SUB_PROJECT_TW}/${PACKAGE}"
echo "       ${OBS_API}/${SUB_PROJECT_FEDORA}/${PACKAGE}"
