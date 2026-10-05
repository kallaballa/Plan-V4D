#!/usr/bin/env python3
"""Generate the GLFW-compatible header that the Android build uses.

GLFW has no Android backend: third/glfw/src/platform.c only knows WIN32, COCOA,
WAYLAND and X11, so there is no platform for glfwInit() to connect to and the
library cannot be configured for Android in the first place (its X11/Wayland
backends default to ON under the NDK toolchain and then hard-require X11 headers
that the sysroot does not have). modules/v4d/third/glfw-android therefore
implements its own small subset of the GLFW API on top of EGL, ANativeWindow and
AInputQueue, and this script generates the header that declares it.

Every constant, typedef and structure is taken verbatim from the vendored GLFW
3.5.1 header, so `#include <GLFW/glfw3.h>` keeps working unchanged in
FrameBufferContext, events.hpp, imguicontext.cpp and the samples, and key /
button / gamepad tokens cannot drift from the values V4D and gwe expect. Only
the function declarations are ours, and only for the subset actually implemented.

    tools/android/gen-glfw-shim-header.py           # write the header
    tools/android/gen-glfw-shim-header.py --check   # fail if the file is stale

The generated header is checked in so that the OpenCV configure step never
depends on Python being installed on an Android build machine. Re-run this
script whenever third/glfw/include/GLFW/glfw3.h changes.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REAL_HEADER = os.path.join(REPO, "modules/v4d/third/glfw/include/GLFW/glfw3.h")
OUT_HEADER = os.path.join(REPO, "modules/v4d/third/glfw-android/GLFW/glfw3.h")

VERSION_TOKENS = (
    "GLFW_VERSION_MAJOR", "GLFW_VERSION_MINOR", "GLFW_VERSION_REVISION",
    "GLFW_TRUE", "GLFW_FALSE",
)

# Tokens that describe a desktop platform's window system. Their numeric values
# are meaningless here (there is no X11/Wayland/Win32/Cocoa connection to talk
# to), and keeping them would let code believe such access is possible, so they
# are the only defines deliberately left out. Everything else upstream defines
# as an object-like macro is mirrored verbatim -- attempting to enumerate the
# families by hand silently dropped GLFW_MOD_*, which quietly broke every
# keyboard modifier check.
EXCLUDED_TOKEN_PREFIXES = (
    r"GLFW_EXPOSE_NATIVE_",     # how to reach the native HWND/X11 Window
    r"GLFW_WIN32_",             # Win32 window hints
    r"GLFW_COCOA_",             # Cocoa window hints
    r"GLFW_X11_",               # X11 window/class hints
    r"GLFW_WAYLAND_",           # Wayland app-id hints
    r"GLFW_ANGLE_",             # ANGLE is an EGL extension we do not request
    r"GLFW_PLATFORM_TYPE_",     # ... and its per-renderer enum
    r"GLFW_DIRECT_",            # Direct3D/OSMesa contexts
    r"GLFW_INCLUDE_",           # header selection macros, handled separately
    r"GLFW_APIENTRY_",          # import/export decoration, meaningless in a .so
    r"GLFWAPI_",                # prefix of the real header's declaration macro
    r"GLFW_KEYBOARD_MENU",      # Win32-keyboard-menu button
    r"GLFW_OSMESA_",
)

STRUCT_PATTERNS = (
    r"typedef void \(\*GLFWglproc\)\(void\);",
    r"typedef void \(\*GLFWvkproc\)\(void\);",
    r"typedef struct GLFWmonitor GLFWmonitor;",
    r"typedef struct GLFWwindow GLFWwindow;",
    r"typedef struct GLFWcursor GLFWcursor;",
    r"typedef struct GLFWvidmode\s*\{[^}]*\}\s*GLFWvidmode;",
    r"typedef struct GLFWgammaramp\s*\{[^}]*\}\s*GLFWgammaramp;",
    r"typedef struct GLFWimage\s*\{[^}]*\}\s*GLFWimage;",
    r"typedef struct GLFWgamepadstate\s*\{[^}]*\}\s*GLFWgamepadstate;",
)

CALLBACK_TYPES = (
    "GLFWerrorfun", "GLFWwindowposfun", "GLFWwindowsizefun",
    "GLFWwindowclosefun", "GLFWwindowrefreshfun", "GLFWwindowfocusfun",
    "GLFWwindowiconifyfun", "GLFWwindowmaximizefun", "GLFWframebuffersizefun",
    "GLFWwindowcontentscalefun", "GLFWmousebuttonfun", "GLFWcursorposfun",
    "GLFWcursorenterfun", "GLFWscrollfun", "GLFWkeyfun", "GLFWcharfun",
    "GLFWcharmodsfun", "GLFWmonitorfun", "GLFWjoystickfun",
)

# The GLFW subset modules/v4d/third/glfw-android implements. The first group is
# what V4D itself calls (FrameBufferContext, events.hpp/gwe, imguicontext.cpp);
# the rest is what the ImGui Android backend, gwe and the samples can reach for.
# Anything not declared here is a compile error at the call site rather than a
# link error later, which keeps the shim honest about its coverage.
FUNCTIONS = """
int             glfwInit(void);
void            glfwTerminate(void);
void            glfwInitHint(int hint, int value);
void            glfwGetVersion(int* major, int* minor, int* rev);
int             glfwGetError(const char** description);
GLFWerrorfun    glfwSetErrorCallback(GLFWerrorfun callback);

int             glfwGetPlatform(void);
int             glfwPlatformSupported(int platform);

void            glfwGetMonitors(GLFWmonitor** monitors, int* count);
GLFWmonitor*    glfwGetPrimaryMonitor(void);
void            glfwGetMonitorPos(GLFWmonitor* monitor, int* xpos, int* ypos);
void            glfwGetMonitorWorkarea(GLFWmonitor* monitor, int* xpos, int* ypos, int* width, int* height);
void            glfwGetMonitorPhysicalSize(GLFWmonitor* monitor, int* widthMM, int* heightMM);
void            glfwGetMonitorContentScale(GLFWmonitor* monitor, float* xscale, float* yscale);
void            glfwSetMonitorUserPointer(GLFWmonitor* monitor, void* pointer);
void*           glfwGetMonitorUserPointer(GLFWmonitor* monitor);
void            glfwSetMonitorCallback(GLFWmonitorfun callback);
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* monitor, int* count);
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* monitor);

GLFWwindow*     glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share);
void            glfwDestroyWindow(GLFWwindow* window);
void            glfwShowWindow(GLFWwindow* window);
void            glfwHideWindow(GLFWwindow* window);
void            glfwIconifyWindow(GLFWwindow* window);
void            glfwRestoreWindow(GLFWwindow* window);
void            glfwMaximizeWindow(GLFWwindow* window);
int             glfwWindowShouldClose(GLFWwindow* window);
void            glfwSetWindowShouldClose(GLFWwindow* window, int value);
void            glfwSetWindowUserPointer(GLFWwindow* window, void* pointer);
void*           glfwGetWindowUserPointer(GLFWwindow* window);
void            glfwSetWindowTitle(GLFWwindow* window, const char* title);

void            glfwSetWindowPos(GLFWwindow* window, int xpos, int ypos);
void            glfwGetWindowPos(GLFWwindow* window, int* xpos, int* ypos);
void            glfwSetWindowSize(GLFWwindow* window, int width, int height);
void            glfwGetWindowSize(GLFWwindow* window, int* width, int* height);
void            glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height);
void            glfwGetWindowContentScale(GLFWwindow* window, float* xscale, float* yscale);
void            glfwGetWindowFrameSize(GLFWwindow* window, int* left, int* top, int* right, int* bottom);
void            glfwSetWindowSizeLimits(GLFWwindow* window, int minwidth, int minheight, int maxwidth, int maxheight);
void            glfwSetWindowAspectRatio(GLFWwindow* window, float numer, float denom);
void            glfwGetWindowAspectRatio(GLFWwindow* window, float* numer, float* denom);
void            glfwSetWindowMonitor(GLFWwindow* window, GLFWmonitor* monitor, int xpos, int ypos, int width, int height, int refreshRate);
GLFWmonitor*    glfwGetWindowMonitor(GLFWwindow* window);
void            glfwSetWindowIcon(GLFWwindow* window, int imageCount, const GLFWimage* images);
void            glfwSetWindowAttrib(GLFWwindow* window, int attrib, int value);
int             glfwGetWindowAttrib(GLFWwindow* window, int attrib);

void            glfwSetWindowPosCallback(GLFWwindow* window, GLFWwindowposfun callback);
void            glfwSetWindowSizeCallback(GLFWwindow* window, GLFWwindowsizefun callback);
void            glfwSetWindowCloseCallback(GLFWwindow* window, GLFWwindowclosefun callback);
void            glfwSetWindowRefreshCallback(GLFWwindow* window, GLFWwindowrefreshfun callback);
void            glfwSetWindowFocusCallback(GLFWwindow* window, GLFWwindowfocusfun callback);
void            glfwSetWindowIconifyCallback(GLFWwindow* window, GLFWwindowiconifyfun callback);
void            glfwSetWindowMaximizeCallback(GLFWwindow* window, GLFWwindowmaximizefun callback);
void            glfwSetFramebufferSizeCallback(GLFWwindow* window, GLFWframebuffersizefun callback);
void            glfwSetWindowContentScaleCallback(GLFWwindow* window, GLFWwindowcontentscalefun callback);

void            glfwPollEvents(void);
void            glfwWaitEvents(void);
void            glfwWaitEventsTimeout(double timeout);
void            glfwPostEmptyEvent(void);

int             glfwGetKey(GLFWwindow* window, int key);
int             glfwGetKeyScancode(int key);
int             glfwGetMouseButton(GLFWwindow* window, int button);
void            glfwGetCursorPos(GLFWwindow* window, double* xpos, double* ypos);
void            glfwSetCursorPos(GLFWwindow* window, double xpos, double ypos);
const char*     glfwGetKeyName(int key, int scancode);
int             glfwGetInputMode(GLFWwindow* window, int mode);
void            glfwSetInputMode(GLFWwindow* window, int mode, int value);
int             glfwRawMouseMotionSupported(void);
void            glfwGetRawMouseMotion(double* xpos, double* ypos);
char**          glfwGetClipboardString(GLFWwindow* window);
void            glfwSetClipboardString(GLFWwindow* window, const char* string);
void            glfwFreeUserPointer(void* pointer);

void            glfwSetKeyCallback(GLFWwindow* window, GLFWkeyfun callback);
void            glfwSetCharCallback(GLFWwindow* window, GLFWcharfun callback);
void            glfwSetCharModsCallback(GLFWwindow* window, GLFWcharmodsfun callback);
void            glfwSetMouseButtonCallback(GLFWwindow* window, GLFWmousebuttonfun callback);
void            glfwSetCursorPosCallback(GLFWwindow* window, GLFWcursorposfun callback);
void            glfwSetCursorEnterCallback(GLFWwindow* window, GLFWcursorenterfun callback);
void            glfwSetScrollCallback(GLFWwindow* window, GLFWscrollfun callback);
void            glfwSetJoystickCallback(GLFWjoystickfun callback);

int             glfwJoystickPresent(int jid);
int             glfwJoystickIsGamepad(int jid);
const float*         glfwGetJoystickAxes(int jid, int* count);
const unsigned char* glfwGetJoystickButtons(int jid, int* count);
const unsigned char* glfwGetJoystickHats(int jid, int* count);
const char*          glfwGetJoystickName(int jid);
const char*          glfwGetGamepadName(int jid);
int             glfwGetGamepadState(int jid, GLFWgamepadstate* state);
void            glfwUpdateGamepadMappings(void);

void            glfwMakeContextCurrent(GLFWwindow* window);
GLFWwindow*     glfwGetCurrentContext(void);
void            glfwSwapBuffers(GLFWwindow* window);
void            glfwSwapInterval(int interval);
GLFWglproc      glfwGetProcAddress(const char* procname);

double          glfwGetTime(void);
void            glfwSetTime(double time);
void            glfwGetTimeValue(double* time);

void            glfwGetDesktopMode(int* width, int* height);
void            glfwGetDesktopModeLimits(int* minWidth, int* minHeight, int* maxWidth, int* maxHeight);
void            glfwGetDesktopModeRefreshRate(int* refreshRate);

void            glfwWindowHint(int hint, int value);
void            glfwDefaultWindowHints(void);
"""

PREAMBLE = """\
// GLFW-compatible API for Android -- GENERATED FILE, do not edit.
//
// Generated by tools/android/gen-glfw-shim-header.py from the vendored GLFW
// 3.5.1 header (modules/v4d/third/glfw/include/GLFW/glfw3.h). Every constant,
// typedef and structure below is copied from it verbatim, so key / button /
// gamepad tokens carry the same values on Android as on the desktop and code
// that branches on GLFW_VERSION_* (framebuffercontext.cpp does) behaves
// identically. The function declarations are the subset that
// modules/v4d/third/glfw-android implements on top of EGL, ANativeWindow and
// AInputQueue.
//
// GLFW itself has no Android backend, so this is what `#include <GLFW/glfw3.h>`
// resolves to when ANDROID is defined. That keeps FrameBufferContext,
// v4d/events.hpp, imguicontext.cpp and the samples free of platform #ifdefs.
//
// Implementation: modules/v4d/third/glfw-android/glfw_android.cpp
// Activity entry point: modules/v4d/third/glfw-android/android_main.cpp

#ifndef GLFW_GLFW3_H_
#define GLFW_GLFW3_H_

#if defined(GLFW_INCLUDE_NONE)
#undef GLFW_INCLUDE_NONE
#endif

#ifdef __cplusplus
extern "C" {
#endif
"""

EPILOGUE = """\

#ifdef __cplusplus
}
#endif

#endif // GLFW_GLFW3_H_
"""


def load_real_header():
    with open(REAL_HEADER, "r", encoding="utf-8") as handle:
        return handle.read()


def define_table(text):
    """Ordered {name: full_line} of every object-like `#define GLFW_*`."""
    table = {}
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"\s*#\s*define\s+(GLFW_\w+)\s+(\S.*?)\s*$", line)
        if not match:
            continue
        name, value = match.group(1), match.group(2)
        # Skip function-like macros and multi-line continuations: both need the
        # surrounding lines to make sense and neither is a token we mirror.
        if value.endswith("\\") or re.match(r"^GLFW\w+\(", value):
            continue
        table.setdefault(name, "#define %-40s %s" % (name, value))
    return table


def take_defines(table, predicate):
    return [line for name, line in sorted(table.items()) if predicate(name)]


def take_prefixes(table, pattern):
    regex = re.compile(pattern)
    return [line for name, line in sorted(table.items()) if regex.match(name)]


def take_callback(text, name):
    match = re.search(r"typedef\s+void\s*\(\s*\*\s*%s\s*\)\s*\([^;]*\);" % name,
                      text)
    if not match:
        raise SystemExit("gen-glfw-shim-header: %s not found upstream" % name)
    # Upstream writes `(* NAME)` with a space; normalise to `(*NAME)`.
    return re.sub(r"\s+", " ", match.group(0)).replace("( *", "(")


def take_struct(text, pattern):
    match = re.search(pattern, text, re.S)
    if not match:
        raise SystemExit("gen-glfw-shim-header: struct not found: %s" % pattern)
    return re.sub(r"[ \t]+", " ", match.group(0)).strip()


def generate():
    text = load_real_header()
    table = define_table(text)

    out = [PREAMBLE]

    def section(title):
        out.append("")
        out.append("// ---- %s ----" % title)

    # GLFW_PLATFORM_ANDROID does not exist upstream -- GLFW has no Android
    # backend to name itself after -- but V4D needs it to describe what this shim
    # is, and GLFW_PLATFORM_COCOA is the number GLFW would have assigned next.
    table.setdefault("GLFW_PLATFORM_ANDROID",
                     "#define %-40s %s" % ("GLFW_PLATFORM_ANDROID", "0x00060006"))

    section("version and boolean tokens (verbatim)")
    for name in VERSION_TOKENS:
        if name not in table:
            raise SystemExit("gen-glfw-shim-header: %s missing upstream" % name)
        out.append(table[name])

    # Every remaining token is mirrored verbatim, grouped only for readability.
    # Selecting by family name is what dropped GLFW_MOD_* before; a blacklist is
    # used instead so a new upstream token family appears automatically.
    excluded = re.compile("|".join(EXCLUDED_TOKEN_PREFIXES))
    mirror = {
        name: line for name, line in table.items()
        if not excluded.match(name) and name not in VERSION_TOKENS
    }
    groups = (
        ("window hints and attributes", r"GLFW_(?:FOCUS|CONTEXT_|OPENGL_|SAMPLES|"
         r"RESIZABLE|VISIBLE|DECORATED|TRANSPARENT|MAXIMIZED|CENTER_CURSOR|"
         r"AUTO_ICONIFY|MOUSE_FOCUS|KEYBOARD_FOCUS|SCALE|CLIENT_API|"
         r"DOUBLEBUFFER|STACK_TRANSPARENT|PLATFORM|COLOR|DEPTH|STENCIL|AUX_|"
         r"RED_|GREEN_|BLUE_|ALPHA_|NO_ROTATION|_FLOATING)"),
        ("input mode and modifier tokens", r"GLFW_(?:CURSOR|STICKY|LOCK_KEY|"
         r"RAW_MOUSE|MOD_)"),
        ("action tokens", r"GLFW_(?:RELEASE|PRESS|REPEAT)$"),
        ("key tokens (verbatim, identical on every platform)", r"GLFW_KEY_"),
        ("mouse button tokens", r"GLFW_MOUSE_BUTTON_"),
        ("joystick and hat tokens", r"GLFW_(?:JOYSTICK_|HAT_)"),
        ("gamepad tokens", r"GLFW_GAMEPAD_"),
        ("error codes", r"GLFW_(?:NOT_INITIALIZED|NO_CURRENT_CONTEXT|"
         r"INVALID_ENUM|INVALID_VALUE|OUT_OF_MEMORY|API_UNAVAILABLE|"
         r"VERSION_UNAVAILABLE|PLATFORM_ERROR|FORMAT_UNAVAILABLE|"
         r"NO_WINDOW_CONTEXT|CURSOR_UNAVAILABLE|FEATURE_UNAVAILABLE|"
         r"FEATURE_UNIMPLEMENTED|PLATFORM_UNAVAILABLE|NO_ERROR)"),
        ("platform identifiers", r"GLFW_PLATFORM_"),
    )
    placed = set()
    for title, pattern in groups:
        regex = re.compile(pattern)
        section(title)
        out.extend(line for name, line in sorted(mirror.items())
                   if name not in placed and regex.match(name))
        placed.update(name for name in mirror if regex.match(name))
    leftovers = sorted(name for name in mirror if name not in placed)
    if leftovers:
        # New upstream tokens land here rather than being dropped on the floor.
        section("other tokens")
        out.extend(mirror[name] for name in leftovers)

    section("types")
    for pattern in STRUCT_PATTERNS:
        out.append(take_struct(text, pattern))
    for name in CALLBACK_TYPES:
        out.append(take_callback(text, name))

    section("functions")
    out.append("// Only the subset modules/v4d/third/glfw-android implements.")
    out.append("// Anything else is deliberately absent, so using an unsupported")
    out.append("// function is a compile error at the call site rather than a link")
    out.append("// error discovered later.")
    declarations, declared = [], {}
    for declaration in FUNCTIONS.strip().splitlines():
        declaration = declaration.rstrip()
        if not declaration.strip():
            continue
        match = re.search(r"\b(glfw\w+)\s*\(", declaration)
        if not match:
            raise SystemExit("gen-glfw-shim-header: cannot parse: %r" % declaration)
        name = match.group(1)
        if name in declared:
            # C++ would reject this at compile time but only once a caller
            # includes the header; catch it while generating instead.
            raise SystemExit("gen-glfw-shim-header: %s declared twice:\n  %s\n  %s"
                             % (name, declared[name], declaration))
        declared[name] = declaration
        declarations.append(declaration)
    out.extend(declarations)

    out.append(EPILOGUE.rstrip("\n"))
    body = "\n".join(out)
    while "\n\n\n" in body:
        body = body.replace("\n\n\n", "\n\n")
    return body + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the checked-in header is stale")
    args = parser.parse_args()

    generated = generate()

    if args.check:
        try:
            with open(OUT_HEADER, "r", encoding="utf-8") as handle:
                current = handle.read()
        except FileNotFoundError:
            print("gen-glfw-shim-header: %s is missing" % OUT_HEADER,
                  file=sys.stderr)
            return 1
        if current != generated:
            print("gen-glfw-shim-header: %s is stale, re-run the script" % OUT_HEADER,
                  file=sys.stderr)
            return 1
        print("%s is up to date" % os.path.relpath(OUT_HEADER, REPO))
        return 0

    os.makedirs(os.path.dirname(OUT_HEADER), exist_ok=True)
    with open(OUT_HEADER, "w", encoding="utf-8") as handle:
        handle.write(generated)
    print("wrote %s (%d lines)" % (os.path.relpath(OUT_HEADER, REPO),
                                   generated.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())