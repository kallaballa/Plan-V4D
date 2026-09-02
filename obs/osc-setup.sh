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
#
# This creates the OBS project hierarchy and does an initial upload of
# packaging files. After this, use regenerate.sh for subsequent updates.

OBS_API="${OBS_API:-https://api.opensuse.org}"
OBS_USER="${1:-${OSC_USER:-}}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGING_DIR="$SCRIPT_DIR/packaging"

# Prevent any interactive editor from opening
EDITOR=true
export EDITOR

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
SUB_PROJECT_UBUNTU="${TOP_PROJECT}:Plan-V4D:Ubuntu_24.04"
SUB_PROJECT_RASPIOS="${TOP_PROJECT}:Plan-V4D:Raspbian_12"
PACKAGE="plan-v4d"

# ---- Create projects ----
# Use temp files for meta XML to avoid heredoc issues with osc

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "--- Creating top-level project: ${TOP_PROJECT} ---"
cat > "$TMPDIR/top.xml" <<XML
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
XML
osc -A "$OBS_API" meta prj "$TOP_PROJECT" --file "$TMPDIR/top.xml" -m "Create Plan-V4D top-level project" 2>/dev/null || true
echo "  Done."

echo ""
echo "--- Creating openSUSE Tumbleweed sub-project: ${SUB_PROJECT_TW} ---"
cat > "$TMPDIR/tw.xml" <<XML
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
XML
osc -A "$OBS_API" meta prj "$SUB_PROJECT_TW" --file "$TMPDIR/tw.xml" -m "Create Tumbleweed sub-project" 2>/dev/null || true
echo "  Done."

echo ""
echo "--- Creating Fedora sub-project: ${SUB_PROJECT_FEDORA} ---"
cat > "$TMPDIR/fedora.xml" <<XML
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
XML
osc -A "$OBS_API" meta prj "$SUB_PROJECT_FEDORA" --file "$TMPDIR/fedora.xml" -m "Create Fedora sub-project" 2>/dev/null || true
echo "  Done."

echo ""
echo "--- Creating Ubuntu 24.04 sub-project: ${SUB_PROJECT_UBUNTU} ---"
cat > "$TMPDIR/ubuntu.xml" <<XML
<project name="${SUB_PROJECT_UBUNTU}">
  <title>Plan-V4D for Ubuntu 24.04</title>
  <description>OpenCV+Plan-V4D packages targeting Ubuntu 24.04 (Noble)</description>
  <person userid="${OBS_USER}" role="maintainer"/>
  <repository name="Ubuntu_24.04">
    <path project="Ubuntu:24.04" repository="universe"/>
    <arch>x86_64</arch>
    <arch>arm64</arch>
  </repository>
  <build>
    <enable/>
  </build>
  <publish>
    <enable/>
  </publish>
</project>
XML
osc -A "$OBS_API" meta prj "$SUB_PROJECT_UBUNTU" --file "$TMPDIR/ubuntu.xml" -m "Create Ubuntu sub-project" 2>/dev/null || true
echo "  Done."

echo ""
echo "--- Creating Raspberry Pi OS 12 sub-project: ${SUB_PROJECT_RASPIOS} ---"
cat > "$TMPDIR/raspbian.xml" <<XML
<project name="${SUB_PROJECT_RASPIOS}">
  <title>Plan-V4D for Raspberry Pi OS (Debian 12)</title>
  <description>OpenCV+Plan-V4D packages targeting Raspberry Pi OS (Debian 12/Bookworm)</description>
  <person userid="${OBS_USER}" role="maintainer"/>
  <repository name="Raspbian_12">
    <path project="Debian:12" repository="standard"/>
    <arch>armhf</arch>
    <arch>arm64</arch>
  </repository>
  <build>
    <enable/>
  </build>
  <publish>
    <enable/>
  </publish>
</project>
XML
osc -A "$OBS_API" meta prj "$SUB_PROJECT_RASPIOS" --file "$TMPDIR/raspbian.xml" -m "Create Raspbian sub-project" 2>/dev/null || true
echo "  Done."

# ---- Create packages and upload initial packaging files ----
# This stages files from the tracked packaging sources (no _service upload)
# After setup, run regenerate.sh to upload the actual source tarballs and trigger builds.

VERSION="4.13.0~beta~kallaballa"
REVISION="1"

generate_deb_files() {
    local target="$1"
    local outdir="$2"
    local srcdir="$PACKAGING_DIR/$target"

    sed -e "s/@VERSION@/$VERSION/g" -e "s/@REVISION@/$REVISION/g" \
        "$srcdir/debian.changelog.in" > "$outdir/debian.changelog"
    cp "$srcdir/debian.control" "$outdir/debian.control"
    cp "$srcdir/debian.rules" "$outdir/debian.rules"
    chmod +x "$outdir/debian.rules"
    (cd "$srcdir" && tar czf "$outdir/debian.tar.gz" debian)
    sed -e "s/@VERSION@/$VERSION/g" -e "s/@REVISION@/$REVISION/g" \
        "$srcdir/plan-v4d.dsc.in" > "$outdir/plan-v4d.dsc"
}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo ""
echo "--- Generating packaging files from tracked sources ---"
generate_deb_files "ubuntu" "$STAGE/ubuntu"
generate_deb_files "raspbian" "$STAGE/raspbian"

for PROJECT in "$SUB_PROJECT_TW" "$SUB_PROJECT_FEDORA" "$SUB_PROJECT_UBUNTU" "$SUB_PROJECT_RASPIOS"; do
    echo ""
    echo "--- Setting up package in ${PROJECT} ---"

    # Create a temporary staging dir for this project
    PKG_DIR="/tmp/obs-${PACKAGE}-$$/$(echo "$PROJECT" | tr ':' '_')"
    mkdir -p "$PKG_DIR"

    if [[ "$PROJECT" == *Raspbian* ]]; then
        cp "$STAGE/raspbian/debian.changelog" "$PKG_DIR/"
        cp "$STAGE/raspbian/debian.control" "$PKG_DIR/"
        cp "$STAGE/raspbian/debian.rules" "$PKG_DIR/"
        cp "$STAGE/raspbian/debian.tar.gz" "$PKG_DIR/"
        cp "$STAGE/raspbian/plan-v4d.dsc" "$PKG_DIR/"
    elif [[ "$PROJECT" == *Ubuntu* ]]; then
        cp "$STAGE/ubuntu/debian.changelog" "$PKG_DIR/"
        cp "$STAGE/ubuntu/debian.control" "$PKG_DIR/"
        cp "$STAGE/ubuntu/debian.rules" "$PKG_DIR/"
        cp "$STAGE/ubuntu/debian.tar.gz" "$PKG_DIR/"
        cp "$STAGE/ubuntu/plan-v4d.dsc" "$PKG_DIR/"
    else
        cp "$SCRIPT_DIR/plan-v4d.spec" "$PKG_DIR/"
    fi

    # Import into OBS
    osc -A "$OBS_API" checkout "$PROJECT/$PACKAGE" 2>/dev/null || true
    if [[ -d "$PROJECT/$PACKAGE" ]]; then
        cp "$PKG_DIR"/* "$PROJECT/$PACKAGE/" 2>/dev/null || true
    else
        mkdir -p "$PROJECT/$PACKAGE"
        cp "$PKG_DIR"/* "$PROJECT/$PACKAGE/" 2>/dev/null || true
    fi

    # Add and commit
    pushd "$PROJECT/$PACKAGE" >/dev/null
    if [[ "$PROJECT" == *Ubuntu* ]] || [[ "$PROJECT" == *Raspbian* ]]; then
        osc -A "$OBS_API" add --force debian.changelog debian.control debian.rules debian.tar.gz plan-v4d.dsc 2>/dev/null || true
    else
        osc -A "$OBS_API" add --force plan-v4d.spec 2>/dev/null || true
    fi
    osc -A "$OBS_API" commit -m "Initial upload: plan-v4d packaging for ${PROJECT##*:}" --noservice
    popd >/dev/null
    echo "  Committed packaging files to ${PROJECT}/${PACKAGE}"
done

# ---- Cleanup ----
rm -rf "/tmp/obs-${PACKAGE}-$$"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Run regenerate.sh to upload source tarballs and trigger builds:"
echo "       ./regenerate.sh"
echo "  2. Monitor builds:"
echo "       ./osc-build.sh"
echo "  3. Or check on web:"
echo "       ${OBS_API}/${SUB_PROJECT_TW}/${PACKAGE}"
echo "       ${OBS_API}/${SUB_PROJECT_FEDORA}/${PACKAGE}"
echo "       ${OBS_API}/${SUB_PROJECT_UBUNTU}/${PACKAGE}"
echo "       ${OBS_API}/${SUB_PROJECT_RASPIOS}/${PACKAGE}"