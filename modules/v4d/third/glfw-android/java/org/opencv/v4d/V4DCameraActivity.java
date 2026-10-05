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
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

public class V4DCameraActivity extends NativeActivity {
    private static final String TAG = "V4D";
    private static final int REQ_CAMERA = 1;

    // super.onCreate() loads the native library, so it must not run twice. The
    // platform can also redeliver onCreate() after a configuration change.
    private boolean nativeStarted = false;
    // Held across the permission round trip: super.onCreate() needs it, and the
    // onRequestPermissionsResult callback has no other copy.
    private Bundle pendingState = null;

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
}