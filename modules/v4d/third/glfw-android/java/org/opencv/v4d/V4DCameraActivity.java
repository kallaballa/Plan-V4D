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
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
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
}