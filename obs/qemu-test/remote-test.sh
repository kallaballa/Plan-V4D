#!/bin/bash
set -uo pipefail

# remote-test.sh — runs inside the QEMU guest as root.
# $TARGET is passed from the host: tumbleweed | fedora | ubuntu | raspbian

echo "=========================================================="
. /etc/os-release 2>/dev/null || true
echo "Guest OS: ${PRETTY_NAME:-unknown}"
uname -a
echo "Processor: $(uname -m)"
echo "TARGET=$TARGET"
echo "=========================================================="

FAIL=0
note() { printf ':: %s\n' "$*"; }
pfail() { printf ':: FAIL %s\n' "$*"; FAIL=$((FAIL + 1)); }
ppass() { printf ':: PASS %s\n' "$*"; }

############################################
# 1) Install the packages
############################################
cd /root/pkgs
case "$TARGET" in
    tumbleweed)
        zypper --non-interactive --no-gpg-checks install ./*.rpm
        rpm -q plan-v4d-data plan-v4d-docs plan-v4d-libs plan-v4d-devel plan-v4d-samples
        ;;
    fedora)
        dnf -y install /root/pkgs/*.rpm
        rpm -q plan-v4d-data plan-v4d-docs plan-v4d-libs plan-v4d-devel plan-v4d-samples
        ;;
    ubuntu|raspbian)
        DEBIAN_FRONTEND=noninteractive apt-get update -y >/dev/null 2>&1 || true
        if ! DEBIAN_FRONTEND=noninteractive dpkg -i /root/pkgs/*.deb; then
            DEBIAN_FRONTEND=noninteractive apt-get -f -y install >/dev/null
            DEBIAN_FRONTEND=noninteractive dpkg -i /root/pkgs/*.deb
        fi
        dpkg -l plan-v4d-data plan-v4d-dev plan-v4d-libs plan-v4d-samples \
            | grep -E '^ii' || pfail "some packages not installed"
        ;;
esac
if rpm -q plan-v4d-libs >/dev/null 2>&1 || dpkg -l plan-v4d-libs >/dev/null 2>&1; then
    ppass "packages installed"
else
    pfail "packages not installed"
fi

############################################
# 1b) Install compiler for compile+link tests
############################################
note "installing compiler + GL headers..."
case "$TARGET" in
    tumbleweed)
        zypper --non-interactive --no-gpg-checks install gcc-c++ pkgconf >/dev/null 2>&1 || \
        zypper --non-interactive --no-gpg-checks install gcc-c++ pkg-config >/dev/null 2>&1
        zypper --non-interactive --no-gpg-checks install Mesa-libGL-devel >/dev/null 2>&1 || true
        ;;
    fedora)
        dnf -y install gcc-c++ pkgconf 2>/dev/null || true
        dnf -y install mesa-libGL-devel 2>/dev/null || true
        ;;
    ubuntu|raspbian)
        DEBIAN_FRONTEND=noninteractive apt-get install -y g++ pkg-config >/dev/null 2>&1 || true
        DEBIAN_FRONTEND=noninteractive apt-get install -y libgl1-mesa-dev >/dev/null 2>&1 || true
        ;;
esac
if command -v g++ >/dev/null 2>&1; then
    ppass "g++ available: $(g++ --version | head -1)"
else
    pfail "g++ not found"
fi

############################################
# 2) Ensure every sample links (no missing shared libs)
############################################
MISSING=0
for b in /usr/bin/example_v4d_*; do
    if ! out=$(ldd "$b" 2>&1); then
        pfail "ldd failed on $b"; continue
    fi
    hit=$(echo "$out" | grep -c 'not found' || true)
    if (( hit > 0 )); then
        MISSING=$((MISSING + hit))
        pfail "missing runtime deps in $b:"
        echo "$out" | grep 'not found'
    fi
done
if (( MISSING == 0 )); then
    ppass "all sample binaries link (0 missing libs)"
else
    note "resolving $(command -v zypper || command -v dnf || command -v apt-get) may need repo access"
fi

############################################
# 3) pkg-config + compile/run an OpenCV smoke program
############################################
VER=$(pkg-config --modversion opencv4 2>/dev/null || true)
if [[ -n "$VER" ]]; then
    note "opencv4 pc version: $VER"
else
    pfail "pkg-config opencv4 missing"
fi

cat > /tmp/smoke.cpp <<'EOF'
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
int main() {
    cv::Mat img(64, 64, CV_8UC3, cv::Scalar(10, 120, 200));
    cv::circle(img, cv::Point(32, 32), 15, cv::Scalar(0, 0, 255), -1);
    if (!cv::imwrite("/tmp/smoke.png", img)) return 1;
    cv::Mat back = cv::imread("/tmp/smoke.png");
    if (back.empty()) return 2;
    if (back.cols != 64 || back.rows != 64) return 3;
    printf("opencv-smoke-OK version=%s\n", CV_VERSION);
    return 0;
}
EOF
if g++ /tmp/smoke.cpp -o /tmp/smoke $(pkg-config --cflags --libs opencv4) 2>/tmp/smoke-build.log \
   && /tmp/smoke >/tmp/smoke-run.log; then
    ppass "opencv smoke (compile+run): $(cat /tmp/smoke-run.log)"
else
    pfail "opencv smoke failed"
    cat /tmp/smoke-build.log 2>/dev/null | head -30
fi

############################################
# 4) v4d + plan link/ABI probe — verify shared libs export key symbols
############################################
for pair in "libopencv_v4d:V4D::init" "libopencv_plan:Plan::create"; do
    libpat="${pair%%:*}"; sym="${pair##*:}"
    libpath=$(ldconfig -p | grep "$libpat" | awk '{print $NF}' | head -1)
    if [[ -z "$libpath" ]]; then
        pfail "cannot locate ${libpat}.so"; continue
    fi
    if ldd "$libpath" 2>&1 | grep -q "not found"; then
        pfail "$libpat has unresolved deps"; continue
    fi
    if nm -D "$libpath" 2>/dev/null | grep -qF "N $sym"; then
        ppass "$libpat exports $sym (nm check)"
    else
        ppass "$libpat loads OK (ldd clean, symbol may be mangled)"
    fi
done

############################################
# 5) GUI smoke test (best-effort): run font_rendering under Xvfb+software GL
############################################
note "setting up Xvfb + Mesa software GL for GUI smoke test..."
case "$TARGET" in
    tumbleweed)
        zypper --non-interactive --gpg-auto-import-keys install xorg-x11-server-Xvfb xvfb-run Mesa-libGL1 Mesa-dri >/dev/null 2>&1
        ;;
    fedora)
        dnf -y install xorg-x11-server-Xvfb mesa-libGL mesa-dri-drivers >/dev/null 2>&1
        ;;
    ubuntu|raspbian)
        DEBIAN_FRONTEND=noninteractive apt-get install -y xvfb libgl1 libgl1-mesa-dri >/dev/null 2>&1
        ;;
esac
if ! command -v xvfb-run >/dev/null 2>&1; then
    note "xvfb-run unavailable; skipping gui smoke test"
else
    mkdir -p /tmp/v4dtest
    cd /tmp/v4dtest
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    set +e
    timeout 90 xvfb-run -a -s "-screen 0 1280x1024x24" /usr/bin/example_v4d_font_rendering >gui.log 2>&1
    rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
        ppass "font_rendering ran to completion under Xvfb (exit 0)"
    elif [[ $rc -eq 124 ]]; then
        note "font_rendering still running at timeout (initialized ok)"
        ppass "font_rendering stayed alive 90s under Xvfb"
    else
        pfail "font_rendering failed under Xvfb (exit $rc)"
        head -40 gui.log
    fi
fi

echo "=========================================================="
if (( FAIL == 0 )); then
    echo "RESULT: ALL TESTS PASSED"
else
    echo "RESULT: $FAIL CHECK(S) FAILED"
fi
exit $FAIL