// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// The one piece of Java in the APK, and the reason the APK has any.
//
// Every video demo in this directory runs on the device camera on Android (see
// Source::makeDefault), and camera capture through OpenCV's Android backend --
// camera2 over the NDK -- needs android.permission.CAMERA. Since Android 6 that
// is a *runtime* permission: declaring it in the manifest only makes it
// requestable, and a process that opens the camera without it is denied. Asking
// for it needs an Activity.
//
// android.app.NativeActivity is already an Activity, so the cheapest thing that
// works is to subclass it rather than add a second one and hand off: the
// manifest keeps naming exactly one library through android.app.lib_name, and
// nothing else about the launch changes. super.onCreate() is what loads the
// demo .so and hands control to ANativeActivity_onCreate(), so it must run
// exactly once, and only after the permission is held.

package org.opencv.v4d;

import android.Manifest;
import android.app.NativeActivity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Display;
import android.view.Surface;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.InputMethodManager;

import java.util.concurrent.LinkedBlockingQueue;

public class V4DCameraActivity extends NativeActivity {
    private static final String TAG = "V4D";
    private static final int REQ_CAMERA = 1;

    // super.onCreate() loads the native library, so it must not run twice. The
    // platform can also redeliver onCreate() after a configuration change.
    private boolean nativeStarted = false;
    // Held across the permission round trip: super.onCreate() needs it, and the
    // onRequestPermissionsResult callback has no other copy.
    private Bundle pendingState = null;

    // Typed Unicode codepoints, filled by dispatchKeyEvent() on the UI thread and
    // drained by native ImGui code on its render thread. A LinkedBlockingQueue
    // is one of the few containers that is safe across the two. 0 is never a
    // meaningful codepoint, so it doubles as "empty".
    private final LinkedBlockingQueue<Integer> unicodeCharacters =
            new LinkedBlockingQueue<>();

    private void startNative(Bundle state) {
        if (nativeStarted) {
            return;
        }
        nativeStarted = true;
        super.onCreate(state);
    }

    private boolean hasCameraPermission() {
        return checkSelfPermission(Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected void onCreate(Bundle state) {
        if (!hasCameraPermission()) {
            // Deliberately not calling super yet: NativeActivity.onCreate() is
            // the thing that loads the .so, and the demo starts capturing on
            // its first frame, which would be before the user answered.
            pendingState = state;
            Log.i(TAG, "Requesting android.permission.CAMERA");
            requestPermissions(new String[] {Manifest.permission.CAMERA}, REQ_CAMERA);
            return;
        }
        startNative(state);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_CAMERA) {
            return;
        }
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            startNative(pendingState);
            return;
        }
        // Denied: there is no fallback input -- the APK ships no video to play --
        // so say so on the log and leave the window empty rather than starting a
        // demo whose first frame can never arrive.
        Log.e(TAG, "android.permission.CAMERA denied; the demo has no other "
                + "video input and will show nothing. Grant it with "
                + "'adb shell pm grant <package> android.permission.CAMERA'.");
        finish();
    }

    // --- Fullscreen ------------------------------------------------------------
    //
    // The demos draw their own chrome right up to the window edge, and the
    // platform now hands an app an edge-to-edge window whether it asks or not:
    // the status bar and the navigation bar are *translucent overlays* on top
    // of it, so ImGui's menu bar ended up underneath the clock and the tab bar
    // under the navigation bar (dumpsys window: StatusBar frame=[0,0][1600,74],
    // NavigationBar0 frame=[1506,0][1600,720], both fmt=TRANSLUCENT, while the
    // app window spans [74,0][1600,720]).
    //
    // Hide both bars so the whole window is the app's. A swipe in from an edge
    // still brings them back transiently (BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE),
    // so the clock and the navigation gestures stay reachable. The display
    // cutout is deliberately left alone: the window keeps avoiding it, which is
    // why the left edge stays clear of the camera hole.
    //
    // Must not be called from onCreate(): Window.getInsetsController()
    // dereferences the decor view, which a NativeActivity has not installed yet
    // at that point, and the resulting NPE kills the launch. onWindowFocusChanged()
    // is the first hook that is guaranteed to run with the window in place.
    private void applyFullscreen() {
        Window window = getWindow();
        if (window == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars()
                        | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            return;
        }
        // API < 30: the flags above do not exist, and setSystemUiVisibility()
        // is still the way. IMMERSIVE_STICKY matches the behaviour above.
        @SuppressWarnings("deprecation")
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        window.getDecorView().setSystemUiVisibility(flags);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // A swipe, a dialog or a resume brings the bars back; hide them again as
        // soon as the window is focused rather than only once at startup.
        if (hasFocus) {
            applyFullscreen();
        }
    }

    // --- On-screen keyboard ----------------------------------------------------
    //
    // Dear ImGui sets io.WantTextInput when a text field is focused, but the NDK
    // cannot show the IME for a NativeActivity: ANativeActivity_showSoftInput()
    // reaches InputMethodManager with a view the IME considers "not served", so
    // it is ignored. The supported route is to do it from Java, which is why
    // these two methods exist; the native GLFW shim calls them by name via JNI.

    /** Called from native code when an ImGui text field gains focus. */
    public void showSoftInput() {
        InputMethodManager imm =
                (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null && getWindow() != null) {
            imm.showSoftInput(getWindow().getDecorView(), 0);
        }
    }

    /** Called from native code when the focused ImGui text field goes away. */
    public void hideSoftInput() {
        InputMethodManager imm =
                (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null && getWindow() != null) {
            imm.hideSoftInputFromWindow(getWindow().getDecorView().getWindowToken(), 0);
        }
    }

    // The NDK exposes no AKeyEvent_getUnicodeChar(), so the IME's characters are
    // captured here and polled by native code (pollUnicodeChar()). The raw event
    // is still handed to super, which delivers it to the native input queue, so
    // ImGui keeps seeing key/backspace/enter as well.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            int codepoint = event.getUnicodeChar(event.getMetaState());
            if (codepoint != 0) {
                unicodeCharacters.offer(codepoint);
            }
        }
        return super.dispatchKeyEvent(event);
    }

    /** Polled from native code; 0 means no character is pending. */
    public int pollUnicodeChar() {
        Integer codepoint = unicodeCharacters.poll();
        return codepoint == null ? 0 : codepoint;
    }

    /** Polled from native code (JNI); returns the current screen rotation as a
     *  Surface.ROTATION_* value (0..3). This is what makes the camera preview
     *  upright however the device is turned: the sensor's fixed mounting
     *  rotation only yields an upright image at the natural orientation. */
    public int getDisplayRotation() {
        Display display = getDisplay();
        return display == null ? Surface.ROTATION_0 : display.getRotation();
    }
}