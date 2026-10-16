#!/usr/bin/env bash
# Fetch and cross-build clvk (OpenCL 3.0 on Vulkan) for one Android ABI.
#
#   tools/android/build-clvk.sh [abi] [-j N] [--configure-only] [--api-level N]
#
# Why this exists: modern Android ships no OpenCL ICD on most devices, and
# OpenCV dlopen()s "libOpenCL.so" at runtime and quietly falls back to its CPU
# kernels when it is not there. clvk (https://github.com/kpet/clvk) is a
# conformant OpenCL 3.0 implementation on top of Vulkan, so bundling
# libOpenCL.so into the APK gives OpenCV a working OpenCL path on every device
# whose GPU exposes Vulkan.
#
# Kernel compilation: OpenCV builds its OpenCL C kernels at runtime with
# clBuildProgram(source), so clvk needs clspv (OpenCL C -> SPIR-V). clvk is
# therefore configured with the in-process "online" compiler
# (CLVK_COMPILER_AVAILABLE=ON + CLVK_CLSPV_ONLINE_COMPILER=ON), which links the
# cross-compiled LLVM/clang, clspv_core and LLVMSPIRVLib into libOpenCL.so -- no
# clspv executable has to ship in the APK, and no exec from the app sandbox is
# needed (modern Android blocks executing binaries in the app data dir anyway).
#
# Sources:
#   modules/v4d/third/clvk is a pinned git submodule. Its own nested submodules
#   (clspv, OpenCL-Headers, SPIRV-Headers, SPIRV-Tools, SPIRV-LLVM-Translator)
#   are checked out here, and LLVM is fetched by clspv's fetch_sources.py at the
#   commit pinned in external/clspv/deps.json -- the same steps clvk's own CI
#   runs for its Android job. The repo already vendors binary downloads at build
#   time this way (see fetch-onnxruntime.sh).
#
# Artifacts (all under $ANDROID_STAGE, default build/android):
#   build/android/clvk-libclc/        host-built libclc bitcode, shared across
#                                     ABIs (it is SPIR, i.e. ABI-independent)
#   build/android/<abi>/clvk/         the CMake/Ninja build tree
#   build/android/<abi>/clvk-prefix/lib/libOpenCL.so
#
# Idempotent: libclc is built once (keyed on the LLVM commit clspv pins), the
# clvk configure is skipped when the build tree already exists, and a repeated
# run resumes the ninja build where it stopped.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

CLVK_DIR="$REPO_DIR/modules/v4d/third/clvk"
STAGE="${ANDROID_STAGE:-$REPO_DIR/build/android}"
ABI="${ANDROID_ABI:-arm64-v8a}"
API_LEVEL="${ANDROID_API_LEVEL:-32}"
JOBS="$(nproc 2>/dev/null || echo 4)"
CONFIGURE_ONLY=0
CLEAN=0

usage() { sed -n '2,40p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --abi)            ABI="$2"; shift 2 ;;
    --api-level)      API_LEVEL="$2"; shift 2 ;;
    -j|--jobs)        JOBS="$2"; shift 2 ;;
    --configure-only) CONFIGURE_ONLY=1; shift ;;
    --clean)          CLEAN=1; shift ;;
    -h|--help)        usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

if [ ! -d "$CLVK_DIR/.git" ]; then
  echo "==> Vendoring the clvk submodule (modules/v4d/third/clvk)"
  git -C "$REPO_DIR" submodule update --init modules/v4d/third/clvk >/dev/null 2>&1 || {
    echo "build-clvk: $CLVK_DIR is missing and could not be initialised." >&2
    echo "  Run: git submodule update --init modules/v4d/third/clvk" >&2
    exit 1
  }
fi
[ -d "$CLVK_DIR" ] || {
  echo "build-clvk: $CLVK_DIR missing." >&2
  echo "  Vendor it first: git submodule update --init modules/v4d/third/clvk" >&2
  exit 1
}

export ANDROID_ABI="$ABI"
export ANDROID_API_LEVEL="$API_LEVEL"
# shellcheck source=tools/android/env.sh
V4D_ANDROID_ENV_QUIET=1 . "$SCRIPT_DIR/env.sh"

# The NDK builds with -j$(nproc) on a 32-core host would exhaust the RAM a
# clang+LLVM cross-build needs (each compile unit is large); cap it and keep
# the big links serialised.
if [ "$JOBS" -gt 8 ]; then
  echo "==> build-clvk: capping jobs $JOBS -> 8 (LLVM cross-build is memory hungry)"
  JOBS=8
fi

BUILD_DIR="$STAGE/$ABI/clvk"
PREFIX="$STAGE/$ABI/clvk-prefix"
LIBCLC_ROOT="$STAGE/clvk-libclc"

# The Vulkan loader lives under the sysroot's ABI lib dir; the NDK ships one
# per API level (its r27 layout is <sysroot>/usr/lib/<triple>/<api>/).
case "$ABI" in
  arm64-v8a)   VK_TRIPLE=aarch64-linux-android ;;
  armeabi-v7a) VK_TRIPLE=arm-linux-androideabi ;;
  x86_64)      VK_TRIPLE=x86_64-linux-android ;;
  x86)         VK_TRIPLE=i686-linux-android ;;
  *) echo "build-clvk: unknown ABI '$ABI'" >&2; exit 1 ;;
esac
VULKAN_LIB="$ANDROID_SYSROOT/usr/lib/$VK_TRIPLE/$API_LEVEL/libvulkan.so"
[ -f "$VULKAN_LIB" ] || {
  echo "build-clvk: $VULKAN_LIB not found (NDK without libvulkan? check --api-level)" >&2
  exit 1
}

if [ "$CLEAN" = 1 ]; then
  echo "==> Removing $BUILD_DIR and $PREFIX"
  rm -rf "$BUILD_DIR" "$PREFIX"
fi
mkdir -p "$PREFIX/lib"

# --- clvk's own dependencies (nested submodules) ---------------------------
# clvk pins clspv/OpenCL-Headers/SPIRV-*/SPIRV-LLVM-Translator as submodules of
# modules/v4d/third/clvk. A plain clone does not carry them; `--recursive` from
# the top-level repo would too, but clvk is only needed for Android, so they
# are fetched here instead of making every checkout pay for them.
if [ -z "$(ls -A "$CLVK_DIR/external/clspv" 2>/dev/null)" ] \
   || [ -z "$(ls -A "$CLVK_DIR/external/OpenCL-Headers" 2>/dev/null)" ]; then
  echo "==> Fetching clvk's nested submodules (clspv, OpenCL-Headers, SPIRV-*)"
  git -C "$CLVK_DIR" submodule update --init --recursive
fi

# --- Android build patch ----------------------------------------------------
# SPIRV-LLVM-Translator's standalone build resets CMAKE_CXX_EXTENSIONS to the
# compiler default in its own project() call, so its translation units get
# -std=gnu++17 while LLVM's precompiled headers were built with -std=c++17.
# patch(1) --forward leaves an already-patched tree alone, so this is
# idempotent and re-applies cleanly after a submodule re-checkout.
SPIRV_TRANSLATOR_DIR="$CLVK_DIR/external/SPIRV-LLVM-Translator"
if [ -f "$SPIRV_TRANSLATOR_DIR/CMakeLists.txt" ]; then
  patch -p1 -d "$SPIRV_TRANSLATOR_DIR" --forward --silent \
    < "$SCRIPT_DIR/clvk-android.patch" || echo "==> Android patch: already applied"
else
  echo "build-clvk: $SPIRV_TRANSLATOR_DIR is empty; submodule init failed" >&2
  exit 1
fi

# --- LLVM (clspv's pinned monorepo, fetched at build time) -----------------
# clspv's fetch_sources.py checks the llvm-project commit out of deps.json into
# external/clspv/third_party/llvm. This is .git-ignored by clspv and not a
# submodule, so it is fetched exactly as clvk's own CI does.
CLSPV_LLVM_ROOT="$CLVK_DIR/external/clspv/third_party/llvm"
CLSPV_LLVM_DIR="$CLSPV_LLVM_ROOT/llvm"
# The monorepo root (deps.json "subdir": "third_party/llvm") is where the .git
# lives; .../llvm/llvm is merely the monorepo's llvm/ subproject and never has a
# .git of its own. Guarding on the subproject made this always true, so the
# multi-GB checkout -- and its git index, which needs index.lock -- was redone on
# every build and any leftover lock aborted the whole Android build.
if [ ! -d "$CLSPV_LLVM_ROOT/.git" ]; then
  echo "==> fetch_sources.py: downloading clspv's LLVM pin (this is a big checkout)"
  (cd "$CLVK_DIR" && python3 external/clspv/utils/fetch_sources.py --shallow --deps llvm)
fi
LLVM_COMMIT="$(python3 -c 'import json;print([c["commit"] for c in json.load(open("'"$CLVK_DIR"'/external/clspv/deps.json"))["commits"] if c["name"]=="llvm"][0])')"

# --- libclc (host build, once, ABI-independent) ----------------------------
# libclc provides the OpenCL builtin implementations (math, convert, images...)
# as SPIR bitcode that clspv links into every kernel at compile time. The
# bitcode is target-independent, so one host build serves every Android ABI;
# even the Android clang that ends up on the device is not needed for it.
# clvk's CI builds it from the same LLVM pin with the clspv--/clspv64-- runtimes
# and hands it to clvk via -DCLSPV_EXTERNAL_LIBCLC_DIR.
LIBCLC_PREFIX="$LIBCLC_ROOT/prefix"
LIBCLC_STAMP="$LIBCLC_PREFIX/.llvm-commit"
if [ -f "$LIBCLC_PREFIX/spirv64-unknown-vulkan/libclc.bc" ] && [ "$(cat "$LIBCLC_STAMP" 2>/dev/null || true)" = "$LLVM_COMMIT" ]; then
  echo "==> libclc already built for LLVM $LLVM_COMMIT (build/android/clvk-libclc)"
else
  echo "==> Building libclc (LLVM $LLVM_COMMIT) -- first run only, host clang build"
  # Only the runtimes bootstrap is reset: the host clang it needs is by far the
  # most expensive part of this step and is target-independent, so it survives.
  rm -rf "$LIBCLC_ROOT/build/runtimes" "$LIBCLC_ROOT/build/projects"
  # libclc's runtime names are target triples whose archs are amdgpu/amdgcn/
  # nvptx64/spirv32/spirv64. With OS "vulkan" libclc emits LLVM bitcode that
  # clspv links into each kernel, and the layout clspv asks for is
  # <dir>/spirv32-unknown-vulkan/libclc.bc (+ spirv64).
  cmake -S "$CLSPV_LLVM_DIR" -B "$LIBCLC_ROOT/build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DRUNTIMES_spirv32-unknown-vulkan_LLVM_ENABLE_RUNTIMES=libclc \
    -DRUNTIMES_spirv64-unknown-vulkan_LLVM_ENABLE_RUNTIMES=libclc \
    -DLLVM_TARGETS_TO_BUILD=Native \
    -DLLVM_RUNTIME_TARGETS="spirv32-unknown-vulkan;spirv64-unknown-vulkan"
  cmake --build "$LIBCLC_ROOT/build" --target libclc -- -j"$JOBS"
  # The runtime output lands under per-runtime subdirectories with no stable
  # name ("spirv32-unknown-vulkan", "spir--", ...); map each libclc.bc to the
  # 32- or 64-bit slot by the runtime name in its path.
  mkdir -p "$LIBCLC_PREFIX/spirv32-unknown-vulkan" "$LIBCLC_PREFIX/spirv64-unknown-vulkan"
  found32=0; found64=0
  while IFS= read -r BC; do
    case "$BC" in
      *clspv64--*|*spirv64*)
        cp -f "$BC" "$LIBCLC_PREFIX/spirv64-unknown-vulkan/libclc.bc"; found64=1 ;;
      *clspv--*|*spirv32*|*spir--)
        cp -f "$BC" "$LIBCLC_PREFIX/spirv32-unknown-vulkan/libclc.bc"; found32=1 ;;
    esac
  done < <(find "$LIBCLC_ROOT/build" -name libclc.bc 2>/dev/null)
  [ "$found32" = 1 ] && [ "$found64" = 1 ] || {
    echo "build-clvk: libclc.bc for both runtimes not found; the runtime target layout changed" >&2
    find "$LIBCLC_ROOT/build" -name libclc.bc 2>/dev/null >&2 || true
    exit 1
  }
  printf '%s' "$LLVM_COMMIT" > "$LIBCLC_STAMP"
  echo "==> libclc ready at $LIBCLC_PREFIX ({spirv32,spirv64}-unknown-vulkan/libclc.bc)"
fi
# Older clvk/clspv revisions expected spir--/spir64--; keep both layouts so the
# pin can move without breaking the script.
if [ -f "$LIBCLC_PREFIX/spirv32-unknown-vulkan/libclc.bc" ]; then
  mkdir -p "$LIBCLC_PREFIX/spir--" "$LIBCLC_PREFIX/spir64--"
  cp -f "$LIBCLC_PREFIX/spirv32-unknown-vulkan/libclc.bc" "$LIBCLC_PREFIX/spir--/libclc.bc"
  cp -f "$LIBCLC_PREFIX/spirv64-unknown-vulkan/libclc.bc" "$LIBCLC_PREFIX/spir64--/libclc.bc"
fi

# --- clvk configure ---------------------------------------------------------
EXTRA_LLVM_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  EXTRA_LLVM_ARGS+=(
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  )
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "==> Configuring clvk for $ABI in $BUILD_DIR"
  # An array, not a continuation chain: a '#' comment line would swallow the
  # trailing backslash of the option above it and silently drop every option
  # after it (the clspv online compiler would then be off and the library
  # useless for OpenCV's runtime kernel builds).
  CMAKE_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE"
    -DANDROID_ABI="$ABI"
    -DANDROID_PLATFORM="android-$API_LEVEL"
    -DANDROID_STL="${ANDROID_STL:-c++_static}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POLICY_VERSION_MINIMUM=3.24
    # SPIRV-LLVM-Translator sets CXX_STANDARD 17 without CMAKE_CXX_EXTENSIONS,
    # so its translation units get -std=gnu++17 while LLVM's precompiled headers
    # are strict c++17: "GNU extensions was disabled in PCH file but is currently
    # enabled". Strict C++17 everywhere matches the PCH on both sides (the vendor
    # patch in clvk-android.patch covers the nested project() reset).
    -DCMAKE_CXX_EXTENSIONS=OFF
    -DCLVK_VULKAN_IMPLEMENTATION=system
    -DVulkan_LIBRARY="$VULKAN_LIB"
    -DVulkan_INCLUDE_DIRS="$ANDROID_SYSROOT/usr/include"
    -DCLVK_BUILD_TESTS=OFF
    -DCLVK_UNIT_TESTING=OFF
    -DCLVK_ENABLE_ASSERTIONS=OFF
    # OpenCV builds its OpenCL C kernels at runtime with clBuildProgram, which
    # is exactly what clvk needs the in-process clspv for.
    -DCLVK_COMPILER_AVAILABLE=ON
    -DCLVK_CLSPV_ONLINE_COMPILER=ON
    -DCLVK_ENABLE_SPIRV_IL=ON
    -DCLSPV_EXTERNAL_LIBCLC_DIR="$LIBCLC_PREFIX"
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DLLVM_INCLUDE_TESTS=OFF
    -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_INCLUDE_DOCS=OFF
    -DLLVM_ENABLE_TERMINFO=OFF
    -DLLVM_ENABLE_ZLIB=OFF
    -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_ZSTD=OFF
    # 16KB page alignment of the LOAD segments: Android 16 (and 16K-page devices
    # in general) refuse to dlopen a library whose segments are only 4K-aligned.
    # Padding to 0x4000 costs nothing and still loads on 4K-page devices.
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=0x4000"
    "${EXTRA_LLVM_ARGS[@]}"
  )
  cmake -S "$CLVK_DIR" -B "$BUILD_DIR" -GNinja "${CMAKE_ARGS[@]}"
  if [ "$CONFIGURE_ONLY" = 1 ]; then
    echo "==> --configure-only: stopping after clvk configure"
    exit 0
  fi
elif [ "$CONFIGURE_ONLY" = 1 ]; then
  echo "==> clvk build tree already configured; --configure-only exiting"
  exit 0
fi

# --- build ----------------------------------------------------------------
echo "==> Building clvk for $ABI (ninja -j$JOBS; resume-safe)"
cmake --build "$BUILD_DIR" --target OpenCL -- -j"$JOBS"

# --- stage ----------------------------------------------------------------
# The target builds libOpenCL.so.0.1 (VERSION 0.1, SOVERSION 1) plus symlinks.
# An APK cannot hold symlinks, and OpenCV dlopen()s by the bare name, so the
# real file is copied in as libOpenCL.so. The SONAME recorded inside (libOpenCL.so.1)
# is not consulted by dlopen-by-name, so it is harmless.
SO_SRC=""
for cand in "$BUILD_DIR/libOpenCL.so" "$BUILD_DIR/libOpenCL.so.0.1" "$BUILD_DIR/libOpenCL.so.1"; do
  if [ -f "$cand" ]; then SO_SRC="$(readlink -f "$cand")"; break; fi
done
[ -n "$SO_SRC" ] || { echo "build-clvk: libOpenCL.so not produced in $BUILD_DIR" >&2; exit 1; }
cp -f "$SO_SRC" "$PREFIX/lib/libOpenCL.so"
if command -v llvm-strip >/dev/null 2>&1; then
  llvm-strip --strip-unneeded "$PREFIX/lib/libOpenCL.so"
else
  "$ANDROID_NDK/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/bin/llvm-strip" \
    --strip-unneeded "$PREFIX/lib/libOpenCL.so"
fi

echo
echo "==> clvk (OpenCL on Vulkan) for $ABI:"
ls -1sh "$PREFIX/lib/libOpenCL.so"
echo
echo "Next: android-build.sh stages it into build/android/jniLibs/$ABI/,"
echo "      package-apk.sh bundles it into lib/$ABI/ of the APK."