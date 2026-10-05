#!/usr/bin/env bash
# Assemble a signed APK for one V4D demo.
#
#   tools/android/package-apk.sh --demo font_rendering
#   tools/android/package-apk.sh --demo vector_graphics --abi x86_64
#
# An APK is a zip, so this needs no Gradle project: the demo .so is already a
# library that android.app.NativeActivity can load, and the only things missing
# are the manifest that names it, the assets, and a signature.
#
# One APK holds one demo, because the manifest has to name exactly one library.
# Building several is fine (see --demo and OPENCV_V4D_SAMPLES); packaging them
# produces several APKs, each with its own launcher entry.
#
# Output:
#   build/android/apk/<demo>-<abi>.apk
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

ABI="${ANDROID_ABI:-arm64-v8a}"
API_LEVEL="${ANDROID_API_LEVEL:-32}"
DEMO="${V4D_ANDROID_DEMO:-font_rendering}"
# Empty here and derived after parsing; `set -u` would reject the reference
# otherwise.
PACKAGE_ID="${V4D_ANDROID_PACKAGE:-}"
STAGE_ROOT="${ANDROID_STAGE:-$REPO_DIR/build/android}"
# A checked-in debug key would be a real signing key in the repository; this one
# is generated on demand and never leaves the machine.
KEYSTORE="${V4D_KEYSTORE:-$STAGE_ROOT/debug.keystore}"
KEY_ALIAS="${V4D_KEY_ALIAS:-v4d}"
usage() { sed -n '2,18p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --demo)      DEMO="$2"; shift 2 ;;
    --abi)       ABI="$2"; shift 2 ;;
    --api-level) API_LEVEL="$2"; shift 2 ;;
    --package)   PACKAGE_ID="$2"; shift 2 ;;
    --keystore)  KEYSTORE="$2"; shift 2 ;;
    -h|--help)   usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

# Derived after parsing, so --demo and --package both take effect.
# The demo name cannot go in a package name verbatim: half of them carry a hyphen
# (video-demo, cube-demo) and a Java package segment may only hold letters, digits
# and underscores. aapt2 rejects the whole manifest over it, with a message about
# 'package' rather than about the demo name, so the rule lives in package-id.sh,
# which run-demo.sh reads too.
[ -n "$PACKAGE_ID" ] || PACKAGE_ID="$("$SCRIPT_DIR/package-id.sh" "$DEMO")"

export ANDROID_ABI="$ABI"
export ANDROID_API_LEVEL="$API_LEVEL"
# shellcheck source=tools/android/env.sh
V4D_ANDROID_ENV_QUIET=1 . "$SCRIPT_DIR/env.sh"

# env.sh exports the version directory name (e.g. "34.0.0"); a caller may have
# overridden it with an absolute path instead. Accept either.
case "$ANDROID_SDK_BUILD_TOOLS" in
  /*) BUILD_TOOLS="$ANDROID_SDK_BUILD_TOOLS" ;;
  "")  BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/34.0.0" ;;
  *)   BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/$ANDROID_SDK_BUILD_TOOLS" ;;
esac
# aapt2 is needed after all: the package manager does not read a text manifest,
# it reads the binary one aapt2 compiles from it. zipalign and apksigner do the
# last two steps.
for tool in aapt2 zipalign apksigner; do
  [ -x "$BUILD_TOOLS/$tool" ] || {
    echo "package-apk: $BUILD_TOOLS/$tool not found" >&2
    echo "  Install build-tools, or set ANDROID_SDK_BUILD_TOOLS." >&2
    exit 1
  }
done

# The camera permission is a runtime permission, so something has to ask for it,
# and that something is an Activity -- see
# modules/v4d/third/glfw-android/java/org/opencv/v4d/V4DCameraActivity.java. That
# makes this the one APK in the build with Java in it: javac against android.jar,
# then d8 to get the classes.dex aapt2 and the runtime both expect. Without it the
# APK has to be hasCode="false" and cannot ask, which is why this used to be
# documented as a blocker.
command -v javac >/dev/null || {
  echo "package-apk: javac not found; a JDK is needed to build the camera launcher." >&2
  exit 1
}
D8="$BUILD_TOOLS/d8"
[ -x "$D8" ] || D8="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/d8"
[ -x "$D8" ] || {
  echo "package-apk: d8 not found at $BUILD_TOOLS/d8 or in cmdline-tools." >&2
  echo "  Install build-tools, or the Android command line tools." >&2
  exit 1
}

# The compile-time API level may be lower than any platform installed (the NDK
# and the SDK platforms are versioned separately), so the newest android.jar
# present is used regardless of --api-level; --api-level only sets
# targetSdkVersion.
ANDROID_JAR=$(ls -1d "$ANDROID_SDK_ROOT"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1 || true)
[ -n "$ANDROID_JAR" ] || {
  echo "package-apk: no platforms/android-*/android.jar under $ANDROID_SDK_ROOT" >&2
  echo "  Install an SDK platform (sdkmanager 'platforms;android-34')." >&2
  exit 1
}
command -v keytool >/dev/null || {
  echo "package-apk: keytool not found; a JDK is needed to sign the APK." >&2
  exit 1
}

DEMO_SO="libv4ddemo_$DEMO.so"
# android.app.lib_name is the *bare* name, not the file name: NativeActivity
# hands it to Runtime.loadLibrary(), which wraps it as lib<name>.so. Passing
# "libv4ddemo_foo.so" therefore asks the loader for
# "liblibv4ddemo_foo.so.so", which is not in the APK, and the launch dies with
# "Unable to find native library libv4ddemo_foo.so using classloader" -- an
# error that quotes the wrong name and so looks like a missing file.
DEMO_LIB="v4ddemo_$DEMO"
SRC_SO="$STAGE_ROOT/jniLibs/$ABI/$DEMO_SO"
[ -f "$SRC_SO" ] || {
  echo "package-apk: $SRC_SO not found." >&2
  echo "  Build it first: ./build.sh -t android --demo $DEMO" >&2
  exit 1
}

OUT_DIR="$STAGE_ROOT/apk"
WORK="$OUT_DIR/work/$DEMO-$ABI"
rm -rf "$WORK"
mkdir -p "$WORK"

# --- signature -------------------------------------------------------------
# Generated once and reused, so a rebuild can be installed over the previous one
# without uninstalling (a changed key would refuse to replace it).
if [ ! -f "$KEYSTORE" ]; then
  echo "==> Generating $KEYSTORE"
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias "$KEY_ALIAS" -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=V4D Android Demo, OU=OpenCV, O=OpenCV, L=, ST=, C=" >/dev/null
fi

# --- assets ----------------------------------------------------------------
# V4D_ASSET_PATH is compiled in and names build/... paths, which do not exist on
# a device. The samples read their models, fonts and images from here instead;
# android_main.cpp copies them out to internal storage on startup.
ASSET_DIR="$WORK/assets"
mkdir -p "$ASSET_DIR"
for dir in "$REPO_DIR/modules/v4d/samples/data" \
           "$REPO_DIR/modules/v4d/samples/fonts" \
           "$REPO_DIR/modules/v4d/assets"; do
  [ -d "$dir" ] || continue
  (cd "$dir" && find . -type f -exec cp --parents -f {} "$ASSET_DIR/" \;)
done
ASSET_COUNT=$(find "$ASSET_DIR" -type f | wc -l)
# The manifest is the list of everything above, one asset path per line, relative
# to the asset root. android_main.cpp copies exactly what this names out of the
# APK on startup -- the NDK cannot enumerate assets, so it cannot discover them --
# and generating the list here is what keeps the two from drifting apart.
(cd "$ASSET_DIR" && find . -type f ! -name assets.manifest -printf '%P\n' | sort) \
  > "$ASSET_DIR/assets.manifest"
echo "==> Staged $ASSET_COUNT asset files"

# --- jniLibs ---------------------------------------------------------------
# Only the demo: OpenCV, plan, nanovg and the GLFW shim are static, so they are
# already inside it. Carrying a stale copy of another demo here would just make
# the APK bigger.
mkdir -p "$WORK/lib/$ABI"
cp -f "$SRC_SO" "$WORK/lib/$ABI/$DEMO_SO"
for extra in "$STAGE_ROOT/jniLibs/$ABI"/libc++_shared.so; do
  [ -e "$extra" ] && cp -f "$extra" "$WORK/lib/$ABI/"
done

# --- camera launcher ---------------------------------------------------------
# Compiled to classes.dex and added to the APK root. See the note above for why
# there is Java in here at all.
JAVA_SRC="$REPO_DIR/modules/v4d/third/glfw-android/java/org/opencv/v4d/V4DCameraActivity.java"
ACTIVITY_CLASS="org.opencv.v4d.V4DCameraActivity"
[ -f "$JAVA_SRC" ] || {
  echo "package-apk: $JAVA_SRC not found" >&2
  exit 1
}
CLASSES_DIR="$WORK/classes"
mkdir -p "$CLASSES_DIR"
# -nowarn: the android.jar stubs have no bodies, so javac has nothing to say
# about the NativeActivity super call. -source/-target 8 because d8 below only
# reads class files that old; a newer javac default is fine too as long as d8
# accepts it, which --min-api 21 does not guarantee.
javac -nowarn -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
      -d "$CLASSES_DIR" "$JAVA_SRC" 2>&1 | grep -vE 'bootstrap class path|deprecat' || true
CLASS_FILE="$CLASSES_DIR/org/opencv/v4d/V4DCameraActivity.class"
[ -f "$CLASS_FILE" ] || {
  echo "package-apk: javac produced no V4DCameraActivity.class" >&2
  exit 1
}
# The class file, not $CLASSES_DIR: d8 rejects a directory as a program input
# ("Unsupported source file type") and wants files. --lib is android.jar, so that
# d8 can resolve android.app.NativeActivity when it desugars this class against
# it; without it d8 warns about every inherited member type it cannot see.
"$D8" --min-api 21 --lib "$ANDROID_JAR" --output "$WORK" "$CLASS_FILE" >/dev/null
[ -f "$WORK/classes.dex" ] || {
  echo "package-apk: d8 produced no classes.dex" >&2
  exit 1
}

# --- manifest --------------------------------------------------------------
# The activity is V4DCameraActivity, a subclass of android.app.NativeActivity:
# it asks for CAMERA and then lets NativeActivity load the library named in
# <meta-data>, so the launch path is otherwise the one from before. hasCode is
# "true" now, which is what a dex needs.
cat > "$WORK/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$PACKAGE_ID">
  <uses-sdk android:minSdkVersion="21" android:targetSdkVersion="$API_LEVEL" />
  <!-- V4D creates an EGL/GLES3 context directly; the compositor needs the same. -->
  <uses-feature android:glEsVersion="0x00030000" android:required="true" />
  <!-- OpenCV's Android capture backend is camera2 over the NDK, and camera2
       needs this granted, not merely declared. V4DCameraActivity asks for it at
       runtime before the demo starts. Not required: a demo with no camera should
       still install and open a window rather than be filtered out of the store. -->
  <uses-permission android:name="android.permission.CAMERA" />
  <uses-feature android:name="android.hardware.camera.any" android:required="false" />
  <application android:label="V4D $DEMO" android:hasCode="true">
    <activity android:name="$ACTIVITY_CLASS"
        android:label="V4D $DEMO"
        android:exported="true"
        android:configChanges="orientation|keyboardHidden|screenSize|screenLayout|density|uiMode">
      <meta-data android:name="android.app.lib_name" android:value="$DEMO_LIB" />
      <!-- Without MAIN/LAUNCHER the APK installs but has no launcher icon,
           and "am start" has nothing to resolve against. -->
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
EOF

# --- link, add libs, align, sign -------------------------------------------
APK="$OUT_DIR/$DEMO-$ABI.apk"
cd "$WORK"
rm -f "$APK"

# aapt2 link compiles the manifest and copies the assets in. It cannot add
# lib/<abi>/*.so, so those go in afterwards with zip.
"$BUILD_TOOLS/aapt2" link \
  -o "$APK" \
  -I "$ANDROID_JAR" \
  --manifest "$WORK/AndroidManifest.xml" \
  -A "$ASSET_DIR" \
  --min-sdk-version 21 \
  --target-sdk-version "$API_LEVEL" \
  --no-version-vectors

# -0 (stored) for the .so: it is mapped straight out of the APK, and a compressed
# entry cannot be mapped. zipalign -p below then page-aligns it. classes.dex is
# read through mmap as well but must stay where it is, so it is added uncompressed
# too -- the runtime opens /data/app/.../base.apk and expects a dex there.
zip -0 -r -q "$APK" "lib"
zip -0 -q "$APK" classes.dex
"$BUILD_TOOLS/zipalign" -f -p 4 "$APK" "$APK.aligned"
mv -f "$APK.aligned" "$APK"

# v1 as well as v2: v2 alone is enough for a device, but apksigner verify then
# has to parse targetSdkVersion out of the manifest to know that, and falls back
# to demanding META-INF/MANIFEST.MF when it cannot. Signing both keeps the local
# verification honest, and v1 costs a few kilobytes.
"$BUILD_TOOLS/apksigner" sign \
  --ks "$KEYSTORE" --ks-key-alias "$KEY_ALIAS" \
  --ks-pass pass:android --key-pass pass:android \
  --min-sdk-version 21 \
  --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
  "$APK"
"$BUILD_TOOLS/apksigner" verify --min-sdk-version 21 --print-certs "$APK"

echo
echo "==> $APK"
ls -1sh "$APK"
echo
echo "Install and run it:"
echo "  tools/android/run-demo.sh $DEMO $ABI"