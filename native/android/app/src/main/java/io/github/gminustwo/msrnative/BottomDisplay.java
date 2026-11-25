package io.github.gminustwo.msrnative;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

/**
 * The 3DS's bottom screen on a second display of its own: the lower screen of a dual-screen handheld (the AYN Thor's,
 * a presentation display beside the default one). The game asks for it (the bottom_display option, video.c's bottom
 * output); its window is a {@link Presentation} on that display, a SurfaceView the game draws the bottom screen into
 * (bottomSurface) and whose touches are the 3DS's touch screen (bottomTouch).
 *
 * The window never takes the focus (FLAG_NOT_FOCUSABLE): a focusable second window takes the controller's input away
 * from the game's while the lower screen is touched. It is shown while the activity is resumed, and taken down before
 * the activity pauses, while the game's thread still runs to let go of the surface (SDL holds that thread on a pause).
 */
final class BottomDisplay {
    private final Activity activity;
    private final DisplayManager displays;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private Screen screen;
    private boolean wanted, resumed = true;

    BottomDisplay(Activity activity) {
        this.activity = activity;
        displays = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
        displays.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { update(); }
            @Override public void onDisplayChanged(int id) { if (screen == null) update(); }
            @Override public void onDisplayRemoved(int id) {
                if (screen != null && screen.getDisplay().getDisplayId() == id) hide();
                update();
            }
        }, ui);
    }

    /** From the game (android.c msr_android_bottom_display, any thread): the bottom screen wanted there or not. */
    void want(boolean on) {
        ui.post(() -> { wanted = on; update(); });
    }

    void onResume() { resumed = true; update(); }

    /** Before the activity's own onPause: the surface is let go of while the game's thread still runs. */
    void onPause() { resumed = false; hide(); }

    /** A presentation display other than the activity's, switched on; null when there is none. */
    private Display pick() {
        final Display own = activity.getWindowManager().getDefaultDisplay();
        for (Display d : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (d.getDisplayId() != own.getDisplayId() && d.getState() != Display.STATE_OFF) return d;
        }
        return null;
    }

    private void update() {
        if (!wanted || !resumed || activity.isFinishing()) { hide(); return; }
        if (screen != null) return;
        final Display display = pick();
        if (display == null) return;
        final Screen s = new Screen(activity, display);
        try {
            s.show();
        } catch (WindowManager.InvalidDisplayException e) {
            System.err.println("msr: the bottom screen's display " + display.getDisplayId() + ": " + e);
            return;
        }
        screen = s;
        System.err.println("msr: the bottom screen on display " + display.getDisplayId() + " (" + display.getName() + ")");
    }

    private void hide() {
        if (screen == null) return;
        final Screen s = screen;
        screen = null;
        s.dismiss();   // (surfaceDestroyed first: the game lets go of the surface)
    }

    /** The window on the second display: a SurfaceView filling it. */
    private static final class Screen extends Presentation {
        Screen(Context context, Display display) { super(context, display); }

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            final SurfaceView view = new SurfaceView(getContext());
            view.setBackgroundColor(Color.TRANSPARENT);
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder holder) {}
                @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                    bottomSurface(holder.getSurface(), w, h);
                }
                @Override public void surfaceDestroyed(SurfaceHolder holder) {
                    bottomSurface(null, 0, 0);   // returns once the game has let go of it
                }
            });
            view.setOnTouchListener((v, e) -> {
                final int action = e.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE) bottomTouch(1, e.getX(), e.getY());
                else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) bottomTouch(0, e.getX(), e.getY());
                return true;
            });
            setContentView(view);
        }
    }

    /** The game's (android.c): the surface to draw the bottom screen into, its size; null: let go of it now. */
    static native void bottomSurface(Surface surface, int width, int height);
    /** The game's (android.c): a touch on it (down 1, up 0) at that place, in the surface's pixels. */
    static native void bottomTouch(int down, float x, float y);
}
