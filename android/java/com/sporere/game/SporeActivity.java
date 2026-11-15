package com.sporere.game;

import android.content.Context;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.view.ViewGroup;
import android.view.Gravity;
import android.util.TypedValue;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** SDL's activity running the native port (libmain.so: the recompiled engine and its platform layer). */
public class SporeActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "dxvk_d3d9", modernCpu() ? "main" : "main_v80"};
    }

    /**
     * libmain.so is built for ARMv8.2 with RCpc (LDAPR keeps the emulated x86 memory ordering cheap) and LSE atomics;
     * older cores (Cortex-A53/A73, e.g. Snapdragon 6xx) get libmain_v80.so, built for ARMv8.0. Every core must have
     * both features: big.LITTLE phones list each core's features separately.
     */
    private static boolean modernCpu() {
        int cores = 0;
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (String line; (line = r.readLine()) != null;) {
                if (!line.startsWith("Features")) continue;
                List<String> features = Arrays.asList(line.substring(line.indexOf(':') + 1).trim().split("\\s+"));
                if (!features.contains("atomics") || !features.contains("lrcpc")) return false;
                ++cores;
            }
        } catch (IOException e) {
            return false;
        }
        return cores > 0;
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new SporeSurface(context);
    }

    /**
     * The engine runs once per process: a new start of the game gets a fresh process. SDL's onDestroy waits on the UI
     * thread for the game thread to return, which the engine's shutdown can make long enough for an ANR, so a
     * finishing activity ends the process before that.
     */
    @Override
    protected void onDestroy() {
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
        super.onDestroy();
    }

    /**
     * Asks for the display mode with the highest refresh rate at the current resolution. Some phones (HyperOS,
     * ColorOS) run apps that don't ask at 30 Hz to save power, and the game's frames are tied to the refresh rate
     * (vsync): it was stuck at 30 fps there whatever the FPS limit said.
     */
    private void preferFastDisplayMode() {
        try {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode(), best = current;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() == current.getPhysicalWidth()
                        && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) {
                    best = m;
                }
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = best.getRefreshRate();
            getWindow().setAttributes(lp);
            android.util.Log.i("sporere", "display mode " + best.getRefreshRate() + " Hz (was "
                    + current.getRefreshRate() + " Hz)");
        } catch (RuntimeException e) {
            android.util.Log.w("sporere", "display mode: " + e);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The native side loads the bundled Vulkan driver (Turnip, on Adreno) from the extracted libraries.
        try {
            Os.setenv("SPORERE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("SPORERE_CREDIT", SetupActivity.CREDIT, true);
            // the game's language follows the phone's unless one is chosen in the settings (advapi32.cpp)
            Os.setenv("SPORERE_SYSTEM_LOCALE", java.util.Locale.getDefault().toLanguageTag(), true);
        } catch (ErrnoException ignored) {
            // the system Vulkan driver is used
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);  // no dimming while playing
        preferFastDisplayMode();
        screenScale = SporeControls.screenScale(this);
        // Black around a smaller picture (Screen size): on an OLED screen black pixels are off, so the border looks
        // like the phone's own frame instead of grey bars.
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(android.graphics.Color.BLACK));
        if (mLayout != null) mLayout.setBackgroundColor(android.graphics.Color.BLACK);
        if (screenScale < 100 && mLayout != null) {
            // sized before the first layout (from the display), so the surface does not start full screen and shrink
            android.util.DisplayMetrics real = new android.util.DisplayMetrics();
            getWindowManager().getDefaultDisplay().getRealMetrics(real);
            fitSurfaceTo(Math.max(real.widthPixels, real.heightPixels), Math.min(real.widthPixels, real.heightPixels));
            mLayout.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> mLayout.post(this::fitSurface));
        }
        // Spore is played with the mouse: SporeControls turns touches into mouse input (tap, drag, long press,
        // two-finger camera, pinch zoom) and adds Esc, keyboard, FPS and settings buttons.
        SporeControls controls = new SporeControls(this);
        addContentView(controls, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // the WASD stick above the controls: it takes only touches on itself, the rest falls through
        SporeStick stick = new SporeStick(this);
        addContentView(stick, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        controls.setStick(stick);
        // The author's credit: on start, and small in a corner for the whole game (it lets touches through).
        Toast.makeText(this, SetupActivity.CREDIT, Toast.LENGTH_LONG).show();
        TextView mark = new TextView(this);
        mark.setText(SetupActivity.CREDIT);
        mark.setTextSize(TypedValue.COMPLEX_UNIT_SP, 8);  // smaller and fainter: still there, out of the way
        mark.setTextColor(0x59ffffff);
        mark.setShadowLayer(2, 1, 1, 0x66000000);
        mark.setClickable(false);
        mark.setFocusable(false);
        int pad = (int) (6 * getResources().getDisplayMetrics().density);
        mark.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
        addContentView(mark, lp);
    }

    /** Settings -> Screen size, in percent (read on start). */
    private int screenScale = 100;

    /**
     * Screen size below 100%: SDL's surface (the game's picture) gets that share of the screen, centered, black around
     * it, so rounded corners and the camera hole cover nothing. It keeps the aspect ratio the engine renders at (the
     * whole display's, main.cpp), so the picture is not stretched. The touch controls map touches into it.
     */
    private void fitSurface() {
        if (mLayout != null) fitSurfaceTo(mLayout.getWidth(), mLayout.getHeight());
    }

    private void fitSurfaceTo(int pw, int ph) {
        if (mSurface == null || pw <= 0 || ph <= 0) return;
        android.util.DisplayMetrics real = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(real);
        float aspect = Math.max(real.widthPixels, real.heightPixels)
                / (float) Math.max(1, Math.min(real.widthPixels, real.heightPixels));  // landscape
        float w = pw * screenScale / 100f, h = ph * screenScale / 100f;
        if (w / h > aspect) w = h * aspect;
        else h = w / aspect;
        int sw = Math.round(w), sh = Math.round(h);
        int left = (pw - sw) / 2, top = (ph - sh) / 2;
        SporeControls.setSurfaceRect(left, top, sw, sh);
        ViewGroup.LayoutParams cur = mSurface.getLayoutParams();
        if (cur != null && cur.width == sw && cur.height == sh) return;
        android.widget.RelativeLayout.LayoutParams lp = new android.widget.RelativeLayout.LayoutParams(sw, sh);
        lp.addRule(android.widget.RelativeLayout.CENTER_IN_PARENT);
        mSurface.setLayoutParams(lp);
    }

    @Override
    public void onBackPressed() {
        SporeControls.onBack(this);  // the game's own menu / back, instead of leaving the app
    }
}
