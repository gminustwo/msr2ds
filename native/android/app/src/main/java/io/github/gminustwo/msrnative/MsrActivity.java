package io.github.gminustwo.msrnative;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.WindowManager;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * msr-native's app: SDL's activity around the program (libmain.so, native/CMakeLists.txt). Its process runs the
 * launcher (no arguments) or the game (the launcher's Play passes the game's arguments), as the PC's launcher and game
 * are processes of their own; {@link #relaunch} starts the app again with other arguments (native/runtime/android.c).
 */
public class MsrActivity extends SDLActivity {
    static final String EXTRA_ARGS = "io.github.gminustwo.msrnative.ARGS";
    static final int PICK_GPU_DRIVER = 1;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected String[] getArguments() {
        final Intent intent = getIntent();
        final String[] args = intent != null ? intent.getStringArrayExtra(EXTRA_ARGS) : null;
        return args != null ? args : new String[0];
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /**
     * From the launcher (launcher.cpp, its GPU driver section): the system's file picker for a GPU driver package, a .zip
     * as emulators take them. {@link #onActivityResult} unpacks the one chosen ({@link GpuDrivers}).
     */
    public void pickGpuDriver() {
        runOnUiThread(() -> {
            final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/zip", "application/x-zip-compressed", "application/x-zip", "application/octet-stream"});
            try {
                startActivityForResult(intent, PICK_GPU_DRIVER);
            } catch (ActivityNotFoundException e) {
                gpuDriverInstalled("", "This device has no file picker to choose the driver package with.");
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK_GPU_DRIVER) {
            super.onActivityResult(request, result, data);
            return;
        }
        final Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            gpuDriverInstalled("", "");   // the picker closed without a file
            return;
        }
        final File root = new File(getFilesDir(), "gpu_drivers");   // (android.c msr_android_gpu_drivers)
        new Thread(() -> {
            final String[] installed = GpuDrivers.install(getContentResolver(), uri, root);
            gpuDriverInstalled(installed[0], installed[1]);
        }, "msr-gpu-driver").start();
    }

    /** The launcher's (launcher.cpp): the driver's folder name once installed, else "" and why ("": nothing chosen). */
    static native void gpuDriverInstalled(String id, String note);

    /** From the program: the app again, in a new process, with these arguments; this process ends after it. */
    public void relaunch(String[] args) {
        final Intent intent = new Intent(this, RelaunchActivity.class);
        intent.putExtra(EXTRA_ARGS, args);
        intent.putExtra(RelaunchActivity.EXTRA_PID, android.os.Process.myPid());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
    }
}
