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
    private BottomDisplay bottom;   // the 3DS's bottom screen on a second display (BottomDisplay)

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
        bottom = new BottomDisplay(this);
    }

    @Override
    protected void onPause() {
        bottom.onPause();   // first: the game's thread still runs to let go of the bottom screen's surface
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        bottom.onResume();
    }

    /** From the game (video.c, through android.c): the bottom screen on a second display, or not. */
    public void bottomDisplay(boolean on) {
        bottom.want(on);
    }

    /** From the settings (overlay_host.cpp, through android.c): whether there is a display for the bottom screen. */
    public boolean hasBottomDisplay() {
        return bottom.available();
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

    /** From the launcher: a page in the system's browser (the Discord invite, GitHub's sign-in page, an issue). */
    public void openUrl(String url) {
        try {
            final Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
        } catch (ActivityNotFoundException e) {
            System.err.println("msr: no app opens " + url);
        }
    }

    /**
     * From the launcher's bug reports (launcher.cpp, on its worker thread): an HTTPS request with these headers (one a
     * line, "Name: value") and the body in the file bodyPath ("": none); the answer's body written to outPath, error
     * statuses' too. The status, or -1 when there was no answer.
     */
    public int httpRequest(String method, String url, String headers, String bodyPath, String outPath) {
        java.net.HttpURLConnection connection = null;
        try {
            connection = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            connection.setRequestMethod(method);
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(120000);
            for (String line : headers.split("\n")) {
                final int colon = line.indexOf(':');
                if (colon > 0) connection.setRequestProperty(line.substring(0, colon).trim(), line.substring(colon + 1).trim());
            }
            if (!bodyPath.isEmpty()) {
                final File body = new File(bodyPath);
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(body.length());
                try (java.io.InputStream in = new java.io.FileInputStream(body);
                     java.io.OutputStream out = connection.getOutputStream()) {
                    final byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
                }
            }
            final int status = connection.getResponseCode();
            final java.io.InputStream answer = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            try (java.io.OutputStream out = new java.io.FileOutputStream(outPath)) {
                if (answer != null) {
                    final byte[] buf = new byte[65536];
                    for (int n; (n = answer.read(buf)) > 0;) out.write(buf, 0, n);
                    answer.close();
                }
            }
            return status;
        } catch (java.io.IOException | RuntimeException e) {
            System.err.println("msr: " + method + " " + url + ": " + e);
            return -1;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }
}
