package com.sporere.game;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Touch controls for Spore, drawn over SDL's surface. The game is played with the mouse; touches become SDL mouse
 * input in one of two modes (settings):
 *   Direct:   the finger is the cursor. Tap = click, drag = left drag, long press = right click.
 *   Touchpad: a cursor on screen; dragging moves it, tap = click where it is, double tap and hold = left drag,
 *             long press = right click. Easier on Spore's small buttons.
 * In both: two fingers drag with the right button held (camera rotation), pinching turns the wheel (zoom).
 * A menu handle (draggable, so it can sit wherever the current stage leaves room) opens a row of buttons: Esc (the
 * game's menu, "back"), Cam (one-finger drags rotate the camera while on), WASD (the movement stick), Undo (Ctrl+Z, the
 * editors), the keyboard (names) and the gear: the settings panel. Engine settings (resolution, textures, frame limit, battery
 * saver) go to sporere.ini and apply on the next start; the rest apply at once.
 * Each stage gets its own layout (the game mode comes from libmain.so): creature: the WASD stick and ability keys 1-4;
 * tribe and civilization: RMB (taps give orders); space: tool keys 1-4 and RMB; editors: Undo and Redo.
 */
public class SporeControls extends View {
    private static final int LEFT = MotionEvent.BUTTON_PRIMARY, RIGHT = MotionEvent.BUTTON_SECONDARY,
            MIDDLE = MotionEvent.BUTTON_TERTIARY;

    /**
     * The button a two-finger drag holds to turn the camera: the middle one (the wheel) in the tribe and civilization
     * stages, where the right button gives orders and does not turn; the right one elsewhere.
     */
    private int turnButton() {
        return stage == Stage.TRIBE || stage == Stage.CIV || stage == Stage.SPACE ? MIDDLE : RIGHT;
    }
    private static final int DOWN = MotionEvent.ACTION_DOWN, UP = MotionEvent.ACTION_UP, MOVE = MotionEvent.ACTION_MOVE;
    private static final long LONG_PRESS_MS = 500, LEFT_DELAY_MS = 60, DOUBLE_TAP_MS = 280;
    private static final int TEAL = 0xff40d6c8;

    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;

    // ---- settings ----
    /** A row of the settings panel; its arrows (or a tap on the row) step through the values. */
    private static final class Setting {
        final String en, ruLabel, key;
        final String[] names;
        final int[] values;
        final boolean engine;  // written to sporere.ini, applies on the next start
        final boolean ini;     // kept in sporere.ini but applied at once (FPS limit)
        String[] codes;        // written to sporere.ini as these strings instead of the numbers (language)
        int index;

        Setting(String en, String ruLabel, String key, String[] names, int[] values, int def, boolean engine,
                boolean ini) {
            this.en = en;
            this.ruLabel = ruLabel;
            this.key = key;
            this.names = names;
            this.values = values;
            this.engine = engine;
            this.ini = ini;
            index = 0;
            for (int i = 0; i < values.length; ++i) if (values[i] == def) index = i;
        }

        int value() {
            return values[index];
        }

        String label() {
            return ru() ? ruLabel : en;
        }
    }

    private static String[] tr(String[] en, String[] ruNames) {
        return ru() ? ruNames : en;
    }

    // The game's language (Data/Locale folders of the GOG release), each in its own language; Auto follows the phone.
    private static final String[] LANG_CODES = {"auto", "en-us", "en-gb", "ru-ru", "de-de", "fr-fr", "es-es", "it-it",
            "pt-br", "pt-pt", "pl-pl", "nl-nl", "cs-cz", "hu-hu", "sv-se", "da-dk", "no-no", "fi-fi", "el-gr", "ja-jp",
            "ko-kr", "zh-cn", "zh-tw", "th-th"};
    private static final String[] LANG_NAMES = {"Auto", "English (US)", "English (UK)", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
            "Deutsch", "Fran\u00e7ais", "Espa\u00f1ol", "Italiano", "Portugu\u00eas (BR)", "Portugu\u00eas (PT)", "Polski",
            "Nederlands", "\u010ce\u0161tina", "Magyar", "Svenska", "Dansk", "Norsk", "Suomi",
            "\u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac", "\u65e5\u672c\u8a9e", "\ud55c\uad6d\uc5b4",
            "\u7b80\u4f53\u4e2d\u6587", "\u7e41\u9ad4\u4e2d\u6587", "\u0e44\u0e17\u0e22"};

    private static int[] indices(int n) {
        int[] v = new int[n];
        for (int i = 0; i < n; ++i) v[i] = i;
        return v;
    }

    private static final String[] OFF_ON = tr(new String[] {"Off", "On"}, new String[] {"Выкл", "Вкл"});
    // Engine settings (*) go to sporere.ini and apply on the next start; the others at once.
    private final Setting sQuality = new Setting("Quality", "Качество", "gpu_tier",
            tr(new String[] {"Fast", "Balanced", "Pretty"}, new String[] {"Быстро", "Баланс", "Красиво"}),
            new int[] {0, 1, 2}, 0, true, false);
    // The game draws its interface in pixels of its resolution: a higher one is sharper, with a smaller interface.
    private final Setting sResolution = new Setting("Resolution", "Разрешение", "render_height",
            new String[] {"540p", "600p", "720p", "900p", "1080p"}, new int[] {540, 600, 720, 900, 1080}, 600, true,
            false);
    private final Setting sFps = new Setting("FPS limit", "Лимит FPS", "fps_limit",
            tr(new String[] {"Off", "30", "40", "45", "50", "60", "90", "120"},
               new String[] {"Выкл", "30", "40", "45", "50", "60", "90", "120"}),
            new int[] {0, 30, 40, 45, 50, 60, 90, 120}, 60, false, true);
    // The stretch from the render size to the screen: plain, or sharpened (DXVK present shader, dxvk-sharpen.patch).
    private final Setting sSharpen = new Setting("Sharpness", "Резкость", "sharpen",
            tr(new String[] {"Off", "Low", "Medium", "High", "Max"},
               new String[] {"Выкл", "Слабая", "Средняя", "Сильная", "Макс"}),
            new int[] {0, 3, 5, 7, 10}, 5, true, false);
    // Off: the CPU prepares the next frame while the GPU draws (smoother on most phones); On: the game's own wait.
    private final Setting sGpuSync = new Setting("Wait for GPU", "Ждать GPU", "gpu_sync", OFF_ON, new int[] {0, 1}, 0,
            true, false);
    private final Setting sShowFps = new Setting("Show FPS", "Показывать FPS", "ui_fps", OFF_ON, new int[] {0, 1}, 0,
            false, false);
    // The game's picture smaller than the screen, black around it: nothing under rounded corners or the camera hole.
    // SporeActivity sizes SDL's surface with it on start (screenScale).
    private final Setting sScreen = new Setting("Screen size", "Размер экрана", "screen_scale",
            new String[] {"100%", "97%", "94%", "91%", "88%"}, new int[] {100, 97, 94, 91, 88}, 100, true, false);
    private final Setting sMode = new Setting("Touch mode", "Касания", "ui_touchpad",
            tr(new String[] {"Direct", "Touchpad"}, new String[] {"Прямые", "Тачпад"}), new int[] {0, 1}, 0, false,
            false);
    private final Setting sSpeed = new Setting("Camera speed", "Скорость камеры", "ui_speed",
            tr(new String[] {"Slow", "Normal", "Fast"}, new String[] {"Медленно", "Обычно", "Быстро"}),
            new int[] {60, 100, 160}, 100, false, false);
    private final Setting sScale = new Setting("Button size", "Размер кнопок", "ui_scale",
            new String[] {"80%", "100%", "120%", "140%"}, new int[] {80, 100, 120, 140}, 100, false, false);
    private final Setting sAlpha = new Setting("Button opacity", "Видимость кнопок", "ui_alpha",
            new String[] {"40%", "60%", "80%", "100%"}, new int[] {40, 60, 80, 100}, 80, false, false);
    private final Setting sLanguage = new Setting("Language", "\u042f\u0437\u044b\u043a", "locale", LANG_NAMES,
            indices(LANG_CODES.length), 0, true, false);

    {
        sLanguage.codes = LANG_CODES;
    }

    private final Setting sVibrate = new Setting("Vibration", "Вибрация", "ui_vibrate", OFF_ON, new int[] {0, 1}, 1,
            false, false);
    // two columns in the panel: graphics, controls
    private final Setting[] graphics = {sQuality, sResolution, sScreen, sSharpen, sFps, sGpuSync, sShowFps};
    private final Setting[] controlsSettings = {sLanguage, sMode, sSpeed, sScale, sAlpha, sVibrate};
    private final Setting[] settings = {sQuality, sResolution, sScreen, sSharpen, sFps, sGpuSync, sShowFps, sLanguage,
            sMode, sSpeed, sScale, sAlpha, sVibrate};

    /** The Screen size setting in percent (sporere.ini, as the panel writes it), for SporeActivity on start. */
    static int screenScale(Context context) {
        File dir = context.getExternalFilesDir(null);
        String v = readIniFrom(dir == null ? null : new File(dir, "sporere.ini"), "screen_scale");
        try {
            int pct = v == null ? 100 : Integer.parseInt(v);
            return Math.max(50, Math.min(100, pct));
        } catch (NumberFormatException e) {
            return 100;
        }
    }

    // Where SDL's surface sits in this view when Screen size made it smaller (SporeActivity.fitSurface): touches are
    // given to SDL in the surface's own pixels. Zero size: the surface fills the view.
    private static volatile float surfLeft, surfTop, surfWidth, surfHeight;

    static void setSurfaceRect(float left, float top, float width, float height) {
        surfLeft = left;
        surfTop = top;
        surfWidth = width;
        surfHeight = height;
    }
    private final RectF[] settingRects = new RectF[settings.length];
    private boolean engineChanged;

    /** Sets the frame rate cap at once (libmain.so, d3d9.cpp). */
    static native void nativeSetFpsLimit(int fps);

    // ---- layout editor ----
    // Every on-screen control can be moved: the menu handle, the stick, the jump button and six slots for the
    // stage's action buttons (1-4, Order, Select, Undo...). Places are fractions of the screen in prefs "lay_*".
    private static final String[] SLOT_IDS = {"slot0", "slot1", "slot2", "slot3", "slot4", "slot5"};
    private static final float[][] SLOT_DEFAULTS = {{0.94f, 0.30f}, {0.94f, 0.46f}, {0.94f, 0.62f}, {0.85f, 0.38f},
            {0.85f, 0.54f}, {0.85f, 0.70f}};
    private boolean editLayout;
    private String editing;  // the element being dragged
    private final RectF editDone = new RectF(), editReset = new RectF();
    private final RectF panelLayout = new RectF();

    private float layX(String id, float def) {
        return prefs.getFloat("lay_" + id + "_x", def);
    }

    private float layY(String id, float def) {
        return prefs.getFloat("lay_" + id + "_y", def);
    }

    private float scale() {
        return sScale.value() / 100f;
    }

    private int alphaOf(int argb) {
        int a = (argb >>> 24) * sAlpha.value() / 100;
        return (a << 24) | (argb & 0xffffff);
    }

    // ---- side buttons ----
    private static final class Button {
        final String label;
        final Runnable action;
        final RectF rect = new RectF();

        Button(String label, Runnable action) {
            this.label = label;
            this.action = action;
        }
    }

    private final List<Button> buttons = new ArrayList<>();
    private Button pressedButton;
    private boolean camLock;  // one-finger drags hold the right button (camera)
    private SporeStick stick;  // WASD movement stick (creature and tribal stages), shown with the WASD button
    // The menu handle: one round button the player can drag anywhere (Spore's own controls sit in different
    // corners in each stage); tapping it opens a row of buttons. Its place is kept as a fraction of the screen.
    private final RectF handle = new RectF();
    private float handleFx, handleFy;
    private boolean menuOpen, handleDragged;

    void setStick(SporeStick s) {
        stick = s;
    }

    // ---- per-stage layout ----
    // The active game mode (libmain.so reads it from the game once a frame) picks a set of action buttons along the
    // right edge, whether the WASD stick shows (remembered per stage once toggled) and whether taps give orders
    // (right clicks) in the strategy stages.
    private enum Stage { OTHER, CELL, CREATURE, TRIBE, CIV, SPACE, EDITOR }
    private Stage stage = Stage.OTHER;
    private final List<Button> actions = new ArrayList<>();
    private boolean rightTaps;  // Order button: one-finger taps are right clicks (orders to the selected units)
    private boolean boxArmed;   // Select button: the next one-finger drag is a left drag (box selection)
    private String hint;        // what the gestures do in this stage, shown for a few seconds after it starts
    private final Runnable hideHint = () -> {
        hint = null;
        invalidate();
    };

    /** In the creature and space stages a one-finger drag turns the camera (right drag). */
    private boolean dragTurnsCamera() {
        if (camLock) return true;
        if (boxArmed) return false;
        return stage == Stage.CREATURE;
    }

    /**
     * In the space stage the ship flies toward the cursor while the right button is held (the game's own hint:
     * "use the right mouse button to fly"): a one-finger drag holds it with the cursor under the finger.
     */
    private boolean dragFlies() {
        return !camLock && !boxArmed && stage == Stage.SPACE;
    }

    /**
     * In the tribe and civilization stages a one-finger drag moves the map, as in mobile strategy games: the game
     * pans with W/A/S/D, so the keys toward which the view should go are held while the finger is away from where it
     * went down (the map follows the finger).
     */
    private boolean dragPansMap() {
        return !camLock && !boxArmed && (stage == Stage.TRIBE || stage == Stage.CIV);
    }

    private static final int[] PAN_KEYS = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};
    private final boolean[] panHeld = new boolean[4];

    private void panKeys(float dx, float dy) {
        float dead = 18 * density;
        // finger right -> the map moves right -> the view goes left (A); finger down -> the view goes up (W)
        boolean[] want = {dy > dead, dx > dead, dy < -dead, dx < -dead};
        for (int i = 0; i < 4; ++i) {
            if (want[i] == panHeld[i]) continue;
            panHeld[i] = want[i];
            if (want[i]) SDLActivity.onNativeKeyDown(PAN_KEYS[i]);
            else SDLActivity.onNativeKeyUp(PAN_KEYS[i]);
        }
    }

    static boolean ru() {
        return "ru".equals(Locale.getDefault().getLanguage()) || "uk".equals(Locale.getDefault().getLanguage());
    }

    private static String hintFor(Stage s) {
        boolean ru = ru();
        switch (s) {
            case CELL:
                return ru ? "Клетка: тап или веди пальцем — плыть, щипок — зум"
                          : "Cell: tap or drag to swim, pinch to zoom";
            case CREATURE:
                return ru ? "Существо: тап — идти / действовать, палец — камера, стик — ходьба, 1–4 — способности"
                          : "Creature: tap to walk / act, drag to turn the camera, stick to walk, 1-4 abilities";
            case TRIBE:
            case CIV:
                return ru ? "Тап — выбрать, удержание — приказ, палец — двигать карту, два пальца — поворот, «Выбор» — рамка"
                          : "Tap to select, hold to order, drag to move the map, two fingers turn, Select for a box";
            case SPACE:
                return ru ? "Космос: держи палец — лететь туда, тап — выбрать, два пальца — камера, +/− — высота"
                          : "Space: hold a finger to fly there, tap to pick, two fingers turn, +/- altitude";
            case EDITOR:
                return ru ? "Редактор: тяни детали пальцем, два пальца — камера, щипок — зум"
                          : "Editor: drag parts, two fingers turn the camera, pinch to zoom";
            default:
                return null;
        }
    }

    /** The active game mode id (App::cGameModeManager), 0 if none. */
    static native int nativeGameMode();

    private static Stage stageOf(int mode) {
        switch (mode) {
            case 0x1654c00: return Stage.CELL;
            case 0x1654c01: return Stage.CREATURE;
            case 0x1654c02: return Stage.TRIBE;
            case 0x1654c04: return Stage.CIV;
            case 0x1654c05: return Stage.SPACE;
            case 0x0dbdba1: return Stage.EDITOR;
            default: return Stage.OTHER;
        }
    }

    private final Runnable modeTick = new Runnable() {
        @Override
        public void run() {
            Stage s = Stage.OTHER;
            try {
                s = stageOf(nativeGameMode());
            } catch (UnsatisfiedLinkError ignored) {
                // the library is not loaded yet
            }
            if (s != stage) enterStage(s);
            handler.postDelayed(this, 500);
        }
    };

    private void enterStage(Stage s) {
        stage = s;
        rightTaps = false;
        boxArmed = false;
        actions.clear();
        hint = hintFor(s);
        handler.removeCallbacks(hideHint);
        if (hint != null) handler.postDelayed(hideHint, 6000);
        switch (s) {
            case CREATURE:  // abilities on 1-4 (attack or social, the game swaps the bar), WASD + jump on the stick
                for (int k = 1; k <= 4; ++k) actions.add(keyButton(String.valueOf(k), KeyEvent.KEYCODE_0 + k));
                break;
            case TRIBE:
            case CIV:
                actions.add(new Button(ru() ? "Приказ" : "Order", this::toggleRightTaps));
                actions.add(new Button(ru() ? "Выбор" : "Select", this::armBox));
                break;
            case SPACE:  // tools on the number keys, altitude on +/- (main keyboard and keypad: whichever the game reads)
                for (int k = 1; k <= 4; ++k) actions.add(keyButton(String.valueOf(k), KeyEvent.KEYCODE_0 + k));
                actions.add(keysButton("+", KeyEvent.KEYCODE_EQUALS, KeyEvent.KEYCODE_NUMPAD_ADD));
                actions.add(keysButton("\u2212", KeyEvent.KEYCODE_MINUS, KeyEvent.KEYCODE_NUMPAD_SUBTRACT));
                break;
            case EDITOR:
                actions.add(new Button("Undo", SporeControls::undo));
                actions.add(new Button("Redo", SporeControls::redo));
                break;
            default:
                break;
        }
        if (stick != null) {
            // tribe and civilization: the map moves with a finger, the stick only when asked for (WASD button)
            boolean show = prefs.getBoolean("stick2_" + s.name(), s == Stage.CREATURE);
            if (show) stick.setVisibility(View.VISIBLE);
            else if (stick.getVisibility() == View.VISIBLE) stick.hide();
        }
        invalidate();
    }

    private Button keysButton(String label, int... keycodes) {
        return new Button(label, () -> {
            for (int k : keycodes) SDLActivity.onNativeKeyDown(k);
            handler.postDelayed(() -> {
                for (int k : keycodes) SDLActivity.onNativeKeyUp(k);
            }, 250);
        });
    }

    private Button keyButton(String label, int keycode) {
        return new Button(label, () -> {
            SDLActivity.onNativeKeyDown(keycode);
            handler.postDelayed(() -> SDLActivity.onNativeKeyUp(keycode), 120);
        });
    }

    private void toggleRightTaps() {
        rightTaps = !rightTaps;
        invalidate();
    }

    private void armBox() {
        boxArmed = !boxArmed;
        invalidate();
    }

    private Button hitAction(float x, float y) {
        for (Button b : actions) if (b.rect.contains(x, y)) return b;
        return null;
    }

    // ---- gesture state ----
    private enum Mode { NONE, PENDING, LEFT_DRAG, CAM_DRAG, PAN, FLY, CURSOR, TWO_FINGER, BUTTON, PANEL, HANDLE, EDIT }
    private Mode mode = Mode.NONE;
    private float downX, downY, lastX, lastY, lastMidX, lastMidY, lastSpan, wheelAccum;
    private float camX, camY;            // virtual pointer while rotating the camera (moves at the camera speed)
    private float cursorX = -1, cursorY;  // touchpad cursor
    private long lastTapTime;
    private boolean longPressFired, moved;
    private final Runnable longPress = this::onLongPress;
    private final Runnable leftDown = this::startLeft;

    // ---- overlay ----
    private boolean panelOpen;
    private final RectF panel = new RectF(), panelClose = new RectF(), panelSupport = new RectF(), panelGitHub = new RectF(),
            panelCloseBig = new RectF();
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            invalidate();
            if (sShowFps.value() == 1) handler.postDelayed(this, 500);
        }
    };

    /** Frames per second over the last second (libmain.so, d3d9.cpp). */
    static native float nativeFps();

    public SporeControls(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE);
        for (Setting s : settings) {
            if (s.codes != null) {
                String code = readIniString(s.key, s.codes[0]).toLowerCase(java.util.Locale.ROOT);
                for (int i = 0; i < s.codes.length; ++i) if (s.codes[i].equals(code)) s.index = i;
                continue;
            }
            int v = s.engine || s.ini ? readIni(s.key, s.value()) : prefs.getInt(s.key, s.value());
            for (int i = 0; i < s.values.length; ++i) if (s.values[i] == v) s.index = i;
        }
        for (int i = 0; i < settingRects.length; ++i) settingRects[i] = new RectF();
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        handleFx = prefs.getFloat("ui_handle_x", 0.5f);
        handleFy = prefs.getFloat("ui_handle_y", 0.0f);
        buttons.add(new Button("Esc", () -> key(KeyEvent.KEYCODE_ESCAPE)));
        buttons.add(new Button("WASD", () -> {
            if (stick == null) return;
            boolean show = stick.getVisibility() != View.VISIBLE;
            if (show) stick.setVisibility(View.VISIBLE);
            else stick.hide();
            prefs.edit().putBoolean("stick2_" + stage.name(), show).apply();
            invalidate();
        }));
        buttons.add(new Button("Stages", this::unlockStages));
        buttons.add(new Button("⌨", this::toggleKeyboard));
        buttons.add(new Button("⚙", () -> {
            panelOpen = true;
            engineChanged = false;
            invalidate();
        }));
        if (sShowFps.value() == 1) handler.post(fpsTick);
        handler.postDelayed(modeTick, 2000);
    }

    // ---- input to SDL ----
    private static void mouse(int buttons, int action, float x, float y) {
        float w = surfWidth, h = surfHeight;
        if (w > 0 && h > 0) {  // a smaller picture: into its pixels, a touch on the black border at its edge
            x = Math.max(0, Math.min(w - 1, x - surfLeft));
            y = Math.max(0, Math.min(h - 1, y - surfTop));
        }
        SDLActivity.onNativeMouse(buttons, action, x, y, false);
    }

    private static void key(int keycode) {
        SDLActivity.onNativeKeyDown(keycode);
        SDLActivity.onNativeKeyUp(keycode);
    }

    /**
     * Opens every stage for a new game (normally a stage is playable only after reaching it): Spore's own cheat
     * console (Ctrl+Shift+C) with "levels -unlock", typed as text. Meant for the galaxy / main menu.
     */
    private void unlockStages() {
        // Ctrl and Shift stay down while the game handles the C (sent at once, the game saw a plain C: Spore's
        // screenshot key), then the command is typed as text.
        Runnable comboDown = () -> {
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SHIFT_LEFT);
        };
        Runnable comboUp = () -> {
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SHIFT_LEFT);
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
        };
        handler.post(comboDown);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_C), 150);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_C), 350);
        handler.postDelayed(comboUp, 500);
        handler.postDelayed(() -> org.libsdl.app.SporeText.type("levels -unlock"), 1000);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ENTER), 1400);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ENTER), 1550);
        // close the console the same way
        handler.postDelayed(comboDown, 2200);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_C), 2350);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_C), 2550);
        handler.postDelayed(comboUp, 2700);
        android.widget.Toast.makeText(getContext(), "All stages unlocked (cheat console: levels -unlock)",
                android.widget.Toast.LENGTH_LONG).show();
    }

    /** Ctrl+Z: undo in the editors. */
    private static void undo() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
        key(KeyEvent.KEYCODE_Z);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
    }

    /** Ctrl+Y: redo in the editors. */
    private static void redo() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
        key(KeyEvent.KEYCODE_Y);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
    }

    private static void wheel(float steps) {
        SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0, steps, false);
    }

    /**
     * A click the game sees whichever way it reads the mouse. Menus use the window messages, but picking things in
     * the 3D world (stars in the galaxy, creatures, buildings) polls the button state once per frame: a press and
     * release within one frame never shows there. So the cursor moves first (hover), the button goes down a little
     * later and stays down for a few frames (150 ms: several frames even at a low frame rate).
     */
    private void click(float x, float y, int button) {
        mouse(0, MOVE, x, y);
        handler.postDelayed(() -> mouse(button, DOWN, x, y), 60);
        handler.postDelayed(() -> mouse(0, UP, x, y), 210);
    }

    private void buzz(int kind) {
        if (sVibrate.value() == 1) performHapticFeedback(kind);
    }

    private float speed() {
        return sSpeed.value() / 100f;
    }

    private boolean touchpad() {
        return sMode.value() == 1;
    }

    /** The phone keyboard is open for the game's text fields (names, chat, cheat console). */
    private boolean typing;

    private void toggleKeyboard() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm == null) return;
        if (typing && keyboardShown()) {
            typing = false;
            imm.hideSoftInputFromWindow(getWindowToken(), 0);
            imm.restartInput(this);
            return;
        }
        typing = true;
        setFocusableInTouchMode(true);
        requestFocus();
        imm.restartInput(this);  // a fresh connection: the keyboard starts from an empty text
        imm.showSoftInput(this, 0);
    }

    private boolean keyboardShown() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            android.view.WindowInsets insets = getRootWindowInsets();
            if (insets != null) return insets.isVisible(android.view.WindowInsets.Type.ime());
        }
        return typing;
    }

    // ---- the phone keyboard ----
    // Without these the keyboard opened on this view had nothing to type into: Android sends typed text only to a view
    // that says it is a text editor and hands out an InputConnection. The connection is SDL's (SporeText), which turns
    // the keyboard's text into SDL text input (WM_CHAR for the game) and backspaces.

    @Override
    public boolean onCheckIsTextEditor() {
        return typing;
    }

    @Override
    public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo out) {
        if (!typing) return null;
        // No suggestions: creature and planet names are made up, autocorrect would change them. Voice typing still works.
        out.inputType = android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | android.text.InputType.TYPE_TEXT_FLAG_CAP_WORDS;
        // No full-screen keyboard in landscape: the game stays visible above it.
        out.imeOptions = android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | android.view.inputmethod.EditorInfo.IME_FLAG_NO_FULLSCREEN
                | android.view.inputmethod.EditorInfo.IME_ACTION_DONE;
        return org.libsdl.app.SporeText.connection(this);
    }

    // Keys reach the focused view: once the keyboard button gave this view the focus, a physical keyboard's keys (and
    // the keys the phone keyboard sends, like Enter and backspace on an empty text) come here instead of SDL's surface.
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return forwardKey(keyCode, event) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return forwardKey(keyCode, event) || super.onKeyUp(keyCode, event);
    }

    private boolean forwardKey(int keyCode, KeyEvent event) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BACK:  // the activity's back (the game's menu)
            case KeyEvent.KEYCODE_VOLUME_UP:
            case KeyEvent.KEYCODE_VOLUME_DOWN:
            case KeyEvent.KEYCODE_VOLUME_MUTE:
            case KeyEvent.KEYCODE_POWER:
            case KeyEvent.KEYCODE_HOME:
                return false;
            default:
                return SDLActivity.handleKeyEvent(this, keyCode, event, null);
        }
    }

    /** The pointer position a one-finger gesture acts at: the finger, or the touchpad cursor. */
    private float px(float x) {
        return touchpad() ? cursorX : x;
    }

    private float py(float y) {
        return touchpad() ? cursorY : y;
    }

    private void startLeft() {
        if (mode != Mode.PENDING) return;
        handler.removeCallbacks(longPress);
        if (dragPansMap()) {
            mode = Mode.PAN;
            return;
        }
        if (dragFlies()) {
            mode = Mode.FLY;
            mouse(0, MOVE, lastX, lastY);
            mouse(RIGHT, DOWN, lastX, lastY);
            return;
        }
        if (dragTurnsCamera()) {
            mode = Mode.CAM_DRAG;
            camX = px(downX);
            camY = py(downY);
            mouse(0, MOVE, camX, camY);
            mouse(RIGHT, DOWN, camX, camY);
            return;
        }
        mode = Mode.LEFT_DRAG;
        mouse(0, MOVE, px(downX), py(downY));
        mouse(LEFT, DOWN, px(downX), py(downY));
    }

    private void onLongPress() {
        if (mode != Mode.PENDING || moved) return;
        handler.removeCallbacks(leftDown);
        longPressFired = true;
        mode = Mode.NONE;
        buzz(HapticFeedbackConstants.LONG_PRESS);
        click(px(downX), py(downY), RIGHT);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        float x = e.getX(), y = e.getY();
        if (cursorX < 0) {
            cursorX = getWidth() / 2f;
            cursorY = getHeight() / 2f;
        }
        if (editLayout) {
            onEditTouch(action, x, y);
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            longPressFired = false;
            moved = false;
            if (panelOpen) {
                mode = Mode.PANEL;
                return true;
            }
            if (handle.contains(x, y)) {
                mode = Mode.HANDLE;
                handleDragged = false;
                downX = x;
                downY = y;
                return true;
            }
            Button b = menuOpen ? hitButton(x, y) : null;
            if (b == null) b = hitAction(x, y);
            if (b != null) {
                pressedButton = b;
                mode = Mode.BUTTON;
                return true;
            }
            downX = lastX = x;
            downY = lastY = y;
            long now = System.currentTimeMillis();
            boolean doubleTap = touchpad() && now - lastTapTime < DOUBLE_TAP_MS;
            if (doubleTap) {  // touchpad: double tap and hold = drag with the left button
                mode = Mode.PENDING;
                startLeft();
                return true;
            }
            mode = touchpad() && !dragTurnsCamera() ? Mode.CURSOR : Mode.PENDING;
            // direct mode: the cursor goes under the finger at once, so the game sees it hover over what will be
            // tapped for as long as the finger rests (3D picking needs a few frames of hover before the press)
            if (!touchpad()) mouse(0, MOVE, x, y);
            // press-and-hold drags with the left button after a moment (editors, cell); where a drag turns the
            // camera or moves the map it starts only when the finger moves, so a slow tap is still a tap
            if (mode == Mode.PENDING && !touchpad() && !dragTurnsCamera() && !dragPansMap() && !dragFlies())
                handler.postDelayed(leftDown, LEFT_DELAY_MS * 3);
            if (mode == Mode.PENDING && !touchpad() && dragFlies()) {
                handler.postDelayed(leftDown, 250);  // space: holding a finger still flies there too
            } else {
                handler.postDelayed(longPress, LONG_PRESS_MS);
            }
            return true;
        }
        if (mode == Mode.PANEL) {
            if (action == MotionEvent.ACTION_UP) panelTap(x, y);
            return true;
        }
        if (mode == Mode.HANDLE) {
            if (action == MotionEvent.ACTION_MOVE && (handleDragged || Math.hypot(x - downX, y - downY) > 12 * density)) {
                handleDragged = true;
                handleFx = clamp(x / getWidth(), 0, 1);
                handleFy = clamp(y / getHeight(), 0, 1);
                invalidate();
            } else if (action == MotionEvent.ACTION_UP) {
                if (handleDragged) {
                    prefs.edit().putFloat("ui_handle_x", handleFx).putFloat("ui_handle_y", handleFy).apply();
                } else {
                    menuOpen = !menuOpen;
                    buzz(HapticFeedbackConstants.VIRTUAL_KEY);
                }
                mode = Mode.NONE;
                invalidate();
            }
            return true;
        }
        if (mode == Mode.BUTTON) {
            if (action == MotionEvent.ACTION_UP) {
                if (pressedButton != null && pressedButton.rect.contains(x, y)) {
                    buzz(HapticFeedbackConstants.VIRTUAL_KEY);
                    pressedButton.action.run();
                    // the menu stays open (Esc is often pressed twice in a row); the handle closes it, the gear
                    // opens the settings over it
                    invalidate();
                }
                pressedButton = null;
                mode = Mode.NONE;
            }
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getPointerCount() == 2) {
                    handler.removeCallbacks(leftDown);
                    handler.removeCallbacks(longPress);
                    if (mode == Mode.LEFT_DRAG || mode == Mode.CAM_DRAG || mode == Mode.FLY) mouse(0, UP, lastMidX, lastMidY);
                    panKeys(0, 0);
                    mode = Mode.TWO_FINGER;
                    lastMidX = (e.getX(0) + e.getX(1)) / 2;
                    lastMidY = (e.getY(0) + e.getY(1)) / 2;
                    camX = getWidth() / 2f;
                    camY = getHeight() / 2f;
                    lastSpan = span(e);
                    wheelAccum = 0;
                    mouse(0, MOVE, camX, camY);
                    mouse(turnButton(), DOWN, camX, camY);
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                onMove(e, x, y);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                if (mode == Mode.TWO_FINGER) {
                    mouse(0, UP, camX, camY);
                    mode = Mode.NONE;  // the remaining finger does nothing until lifted
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                handler.removeCallbacks(leftDown);
                if (!longPressFired && !moved && (mode == Mode.PENDING || mode == Mode.CURSOR)) {
                    int button = rightTaps ? RIGHT : LEFT;
                    click(px(x), py(y), button);
                    lastTapTime = System.currentTimeMillis();
                    buzz(HapticFeedbackConstants.KEYBOARD_TAP);
                } else if (mode == Mode.LEFT_DRAG) {
                    mouse(0, UP, px(x), py(y));
                    boxArmed = false;  // the Select button arms one box drag
                } else if (mode == Mode.CAM_DRAG || mode == Mode.TWO_FINGER) {
                    mouse(0, UP, camX, camY);
                } else if (mode == Mode.PAN) {
                    panKeys(0, 0);
                } else if (mode == Mode.FLY) {
                    mouse(0, UP, x, y);
                }
                if (!touchpad()) parkIfAtEdge(x, y);
                mode = Mode.NONE;
                invalidate();
                return true;
            default:
                return true;
        }
    }

    /**
     * In the tribal, civilization and space stages the camera scrolls while the mouse rests at a screen edge. With a
     * finger the "mouse" stays where it was lifted, so after a tap near an edge the camera kept sliding: the cursor
     * goes back to the middle of the screen instead.
     */
    private void parkIfAtEdge(float x, float y) {
        float mx = getWidth() * 0.06f, my = getHeight() * 0.08f;
        if (x > mx && x < getWidth() - mx && y > my && y < getHeight() - my) return;
        handler.postDelayed(() -> mouse(0, MOVE, getWidth() / 2f, getHeight() / 2f), 120);
    }

    private void onMove(MotionEvent e, float x, float y) {
        float dx = x - lastX, dy = y - lastY;
        if (!moved && Math.hypot(x - downX, y - downY) > 10 * density) {
            moved = true;
            handler.removeCallbacks(longPress);
        }
        switch (mode) {
            case TWO_FINGER: {
                if (e.getPointerCount() < 2) return;
                float mx = (e.getX(0) + e.getX(1)) / 2, my = (e.getY(0) + e.getY(1)) / 2;
                camX += (mx - lastMidX) * speed();
                camY += (my - lastMidY) * speed();
                lastMidX = mx;
                lastMidY = my;
                mouse(turnButton(), MOVE, camX, camY);
                float s = span(e);
                wheelAccum += (s - lastSpan) / (40 * density);  // one wheel step per 40 dp of pinch
                lastSpan = s;
                while (Math.abs(wheelAccum) >= 1) {
                    float step = Math.signum(wheelAccum);
                    wheel(step);
                    wheelAccum -= step;
                }
                break;
            }
            case CURSOR:
                cursorX = clamp(cursorX + dx * speed(), 0, getWidth() - 1);
                cursorY = clamp(cursorY + dy * speed(), 0, getHeight() - 1);
                mouse(0, MOVE, cursorX, cursorY);
                invalidate();
                break;
            case PENDING:
                if (moved) {
                    handler.removeCallbacks(leftDown);
                    startLeft();
                }
                break;
            case LEFT_DRAG:
                if (touchpad()) {
                    cursorX = clamp(cursorX + dx * speed(), 0, getWidth() - 1);
                    cursorY = clamp(cursorY + dy * speed(), 0, getHeight() - 1);
                    mouse(LEFT, MOVE, cursorX, cursorY);
                    invalidate();
                } else {
                    mouse(LEFT, MOVE, x, y);
                }
                break;
            case CAM_DRAG:
                camX += dx * speed();
                camY += dy * speed();
                mouse(RIGHT, MOVE, camX, camY);
                break;
            case PAN:
                panKeys(x - downX, y - downY);
                break;
            case FLY:
                mouse(RIGHT, MOVE, x, y);
                break;
            default:
                break;
        }
        lastX = x;
        lastY = y;
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    private static float span(MotionEvent e) {
        return (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));
    }

    private Button hitButton(float x, float y) {
        for (Button b : buttons) if (b.rect.contains(x, y)) return b;
        return null;
    }

    // ---- settings panel ----
    private void closePanel() {
        panelOpen = false;
        if (engineChanged) {
            android.widget.Toast.makeText(getContext(), ru() ? "Настройки со * применятся после перезапуска игры"
                    : "Restart the game to apply the * settings", android.widget.Toast.LENGTH_LONG).show();
        }
        invalidate();
    }

    private void panelTap(float x, float y) {
        if (panelClose.contains(x, y) || panelCloseBig.contains(x, y) || !panel.contains(x, y)) {
            closePanel();
            return;
        }
        if (panelSupport.contains(x, y)) {
            SetupActivity.openSupport(getContext());
            return;
        }
        if (panelGitHub.contains(x, y)) {
            SetupActivity.openGitHub(getContext());
            return;
        }
        if (panelLayout.contains(x, y)) {
            closePanel();
            startLayoutEditor();
            return;
        }
        for (int i = 0; i < settings.length; ++i) {
            RectF r = settingRects[i];
            if (!r.contains(x, y)) continue;
            Setting s = settings[i];
            // the value sits in the right part of the row between arrows: the left arrow steps back
            boolean back = x < r.right - r.width() * 0.22f && x > r.right - r.width() * 0.48f;
            int n = s.values.length;
            s.index = back ? (s.index + n - 1) % n : (s.index + 1) % n;
            buzz(HapticFeedbackConstants.KEYBOARD_TAP);
            applySetting(s);
            invalidate();
            return;
        }
    }

    private void applySetting(Setting s) {
        if (s.engine) {
            if (s.codes != null) {
                writeIniString(s.key, s.codes[s.index]);
                // the game folder's file wins: update its line too if it has one
                File game = gameIniFile();
                if (game.exists() && readIniFrom(game, s.key) != null) writeIniTo(game, s.key, s.codes[s.index]);
            }
            else writeIni(s.key, s.value());
            // Quality also picks the texture detail: Pretty loads full textures, the others skip the top mip
            if (s == sQuality) writeIni("texture_skip_mips", s.value() == 2 ? 0 : 1);
            engineChanged = true;
            return;
        }
        if (s.ini) writeIni(s.key, s.value());
        prefs.edit().putInt(s.key, s.value()).apply();
        if (s == sFps) {
            try {
                nativeSetFpsLimit(s.value());
            } catch (UnsatisfiedLinkError ignored) {
                // applies on the next start
            }
        }
        if (s == sShowFps && s.value() == 1) handler.post(fpsTick);
        if ((s == sScale || s == sAlpha) && stick != null) stick.applyLayout();
    }

    // ---- layout editor ----
    private void startLayoutEditor() {
        editLayout = true;
        menuOpen = false;
        SporeStick.editing = true;
        if (stick != null) stick.invalidate();
        invalidate();
    }

    private void stopLayoutEditor() {
        editLayout = false;
        SporeStick.editing = false;
        if (stick != null) stick.applyLayout();
        invalidate();
    }

    /** The editable elements and their places (pixels) right now: id -> {x, y, radius}. */
    private List<Object[]> editElements() {
        List<Object[]> out = new ArrayList<>();
        float w = getWidth(), h = getHeight(), sc = scale();
        out.add(new Object[] {"handle", handleFx * w, handleFy * h, 22 * density * sc, "☰"});
        out.add(new Object[] {"stick", layX("stick", SporeStick.STICK_X) * w, layY("stick", SporeStick.STICK_Y) * h,
                62 * density * sc, ru() ? "Стик" : "Stick"});
        out.add(new Object[] {"jump", layX("jump", SporeStick.JUMP_X) * w, layY("jump", SporeStick.JUMP_Y) * h,
                36 * density * sc, ru() ? "Прыжок" : "Jump"});
        for (int i = 0; i < SLOT_IDS.length; ++i) {
            String label = i < actions.size() ? actions.get(i).label : String.valueOf(i + 1);
            out.add(new Object[] {SLOT_IDS[i], layX(SLOT_IDS[i], SLOT_DEFAULTS[i][0]) * w,
                    layY(SLOT_IDS[i], SLOT_DEFAULTS[i][1]) * h, 29 * density * sc, label});
        }
        return out;
    }

    private void onEditTouch(int action, float x, float y) {
        if (action == MotionEvent.ACTION_DOWN) {
            editing = null;
            if (editDone.contains(x, y)) {
                stopLayoutEditor();
                return;
            }
            if (editReset.contains(x, y)) {
                SharedPreferences.Editor e = prefs.edit();
                for (String k : prefs.getAll().keySet()) if (k.startsWith("lay_")) e.remove(k);
                e.putFloat("ui_handle_x", 0.5f).putFloat("ui_handle_y", 0f).apply();
                handleFx = 0.5f;
                handleFy = 0f;
                buzz(HapticFeedbackConstants.LONG_PRESS);
                invalidate();
                return;
            }
            float best = Float.MAX_VALUE;
            for (Object[] el : editElements()) {
                float d = (float) Math.hypot(x - (float) el[1], y - (float) el[2]);
                if (d < (float) el[3] * 1.3f + 8 * density && d < best) {
                    best = d;
                    editing = (String) el[0];
                }
            }
            return;
        }
        if (editing == null) return;
        float fx = clamp(x / getWidth(), 0.02f, 0.98f), fy = clamp(y / getHeight(), 0.02f, 0.98f);
        if (editing.equals("handle")) {
            handleFx = fx;
            handleFy = fy;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            if (!editing.equals("handle"))
                prefs.edit().putFloat("lay_" + editing + "_x", fx).putFloat("lay_" + editing + "_y", fy).apply();
            invalidate();
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (editing.equals("handle"))
                prefs.edit().putFloat("ui_handle_x", handleFx).putFloat("ui_handle_y", handleFy).apply();
            editing = null;
            invalidate();
        }
    }

    private void drawEditor(Canvas c) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x66000000);
        c.drawRect(0, 0, getWidth(), getHeight(), fill);
        text.setTextAlign(Paint.Align.CENTER);
        for (Object[] el : editElements()) {
            float x = (float) el[1], y = (float) el[2], r = (float) el[3];
            boolean active = el[0].equals(editing);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(active ? 0xc040d6c8 : 0x900a1a2a);
            c.drawCircle(x, y, r, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(2 * density);
            fill.setColor(TEAL);
            c.drawCircle(x, y, r, fill);
            text.setColor(Color.WHITE);
            text.setTextSize(14 * density);
            c.drawText((String) el[4], x, y + 5 * density, text);
        }
        // the bar: what to do, Done and Reset
        float bw = 130 * density, bh = 40 * density, top = 12 * density, gap = 10 * density;
        String title = ru() ? "Перетаскивайте кнопки" : "Drag the controls";
        text.setTextSize(15 * density);
        float tw = text.measureText(title) + 30 * density;
        float total = tw + 2 * (bw + gap), left = (getWidth() - total) / 2;
        RectF t = new RectF(left, top, left + tw, top + bh);
        editReset.set(t.right + gap, top, t.right + gap + bw, top + bh);
        editDone.set(editReset.right + gap, top, editReset.right + gap + bw, top + bh);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0xe00b1626);
        c.drawRoundRect(t, 12 * density, 12 * density, fill);
        fill.setColor(0xff6a3a3a);
        c.drawRoundRect(editReset, 12 * density, 12 * density, fill);
        fill.setColor(0xff1d7a6a);
        c.drawRoundRect(editDone, 12 * density, 12 * density, fill);
        text.setColor(Color.WHITE);
        c.drawText(title, t.centerX(), t.centerY() + 5 * density, text);
        c.drawText(ru() ? "Сброс" : "Reset", editReset.centerX(), editReset.centerY() + 5 * density, text);
        c.drawText(ru() ? "Готово" : "Done", editDone.centerX(), editDone.centerY() + 5 * density, text);
        text.setTextSize(14 * density);
    }

    private File iniFile() {
        File dir = getContext().getExternalFilesDir(null);
        return dir == null ? null : new File(dir, "sporere.ini");
    }

    private String readIniString(String key, String def) {
        String v = readIniFrom(gameIniFile(), key);  // the game folder's file wins, as in the game
        if (v == null) v = readIniFrom(iniFile(), key);
        return v != null ? v : def;
    }

    private static String readIniFrom(File ini, String key) {
        if (ini == null) return null;
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                String[] kv = l.split("=", 2);
                if (kv.length == 2 && kv[0].trim().equalsIgnoreCase(key)) return kv[1].trim();
            }
        } catch (IOException ignored) {
            // no setting yet
        }
        return null;
    }

    private int readIni(String key, int def) {
        File ini = iniFile();
        if (ini == null) return def;
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                String[] kv = l.split("=", 2);
                if (kv.length == 2 && kv[0].trim().equalsIgnoreCase(key)) return Integer.parseInt(kv[1].trim());
            }
        } catch (IOException | NumberFormatException ignored) {
            // no setting yet
        }
        return def;
    }

    private void writeIni(String key, int value) {
        writeIniString(key, String.valueOf(value));
    }

    private void writeIniString(String key, String value) {
        writeIniTo(iniFile(), key, value);
    }

    /** The game folder's sporere.ini (its keys win over the app's: a PC may have put a language there). */
    private File gameIniFile() {
        String game = null;
        try {
            game = android.system.Os.getenv("SPORERE_GAME");
        } catch (RuntimeException ignored) {
            // not set: the game is in the app's folder
        }
        File dir = game != null ? new File(game) : new File(getContext().getExternalFilesDir(null), "game");
        return new File(dir, "sporere.ini");
    }

    private void writeIniTo(File ini, String key, String value) {
        if (ini == null) return;
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                if (!l.split("=", 2)[0].trim().equalsIgnoreCase(key)) lines.add(l);
            }
        } catch (IOException ignored) {
            // a new file
        }
        lines.add(key + " = " + value);
        try (FileWriter w = new FileWriter(ini)) {
            for (String l : lines) w.write(l + "\n");
        } catch (IOException ignored) {
            // the game keeps its defaults
        }
    }

    // ---- drawing ----
    @Override
    protected void onDraw(Canvas c) {
        float size = 44 * density * scale(), gap = 8 * density, r = size / 2;
        float hx = clamp(handleFx * getWidth(), r + 4 * density, getWidth() - r - 4 * density);
        float hy = clamp(handleFy * getHeight(), r + 4 * density, getHeight() - r - 4 * density);
        handle.set(hx - r, hy - r, hx + r, hy + r);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alphaOf(menuOpen ? 0xd040d6c8 : 0x900a1a2a));
        if (!editLayout) c.drawCircle(hx, hy, r, fill);
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(2 * density);
        fill.setColor(TEAL);
        c.drawCircle(hx, hy, r, fill);
        fill.setStyle(Paint.Style.FILL);
        text.setColor(Color.WHITE);
        text.setTextSize(20 * density);
        c.drawText(menuOpen ? "\u00d7" : "\u2630", hx, hy + 7 * density, text);
        // state dots: camera lock, WASD stick
        boolean wasd = stick != null && stick.getVisibility() == View.VISIBLE;
        if (camLock || wasd) {
            fill.setColor(0xffffd24a);
            if (camLock) c.drawCircle(hx - r * 0.55f, hy - r * 0.75f, 4 * density, fill);
            if (wasd) c.drawCircle(hx + r * 0.55f, hy - r * 0.75f, 4 * density, fill);
        }
        // the menu row opens toward the wider side of the screen, beside the handle
        boolean toLeft = hx > getWidth() / 2f;
        float bx = toLeft ? hx - r - gap - size : hx + r + gap;
        float by = hy - r;
        text.setTextSize(14 * density);
        for (Button b : buttons) {
            if (!menuOpen) {
                b.rect.setEmpty();
                continue;
            }
            b.rect.set(bx, by, bx + size, by + size);
            boolean on = (b.label.equals("Cam") && camLock) || (b.label.equals("WASD") && wasd);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(on ? 0xd040d6c8 : 0xc00a1a2a);
            c.drawRoundRect(b.rect, 12 * density, 12 * density, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(1.5f * density);
            fill.setColor(TEAL);
            c.drawRoundRect(b.rect, 12 * density, 12 * density, fill);
            fill.setStyle(Paint.Style.FILL);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 5 * density, text);
            bx += toLeft ? -(size + gap) : size + gap;
        }
        drawActions(c, size, gap);
        float x = 8 * density;
        text.setColor(Color.WHITE);
        if (touchpad() && cursorX >= 0) {  // the touchpad cursor
            fill.setColor(0xccffffff);
            c.drawCircle(cursorX, cursorY, 7 * density, fill);
            fill.setColor(0xff102030);
            c.drawCircle(cursorX, cursorY, 3 * density, fill);
        }
        if (sShowFps.value() == 1) {
            text.setTextSize(13 * density);
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(String.format(Locale.ROOT, "%.0f fps", nativeFps()), x, getHeight() - 10 * density, text);
            text.setTextAlign(Paint.Align.CENTER);
        }
        if (hint != null && !panelOpen) {
            text.setTextSize(15 * density);
            float w = Math.min(getWidth() - 32 * density, text.measureText(hint) + 40 * density);
            float top = 12 * density, h = 40 * density;
            RectF hr = new RectF((getWidth() - w) / 2, top, (getWidth() + w) / 2, top + h);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0xd00a1a2a);
            c.drawRoundRect(hr, 14 * density, 14 * density, fill);
            text.setColor(Color.WHITE);
            c.drawText(hint, hr.centerX(), hr.centerY() + 5 * density, text);
            text.setTextSize(14 * density);
        }
        if (editLayout) drawEditor(c);
        if (panelOpen) drawPanel(c);
    }

    /** The stage's action buttons, each in its layout slot (sized and faded per the settings). */
    private void drawActions(Canvas c, float size, float gap) {
        if (actions.isEmpty() || editLayout) return;
        float r = 29 * density * scale();
        text.setTextSize(16 * density * scale());
        for (int i = 0; i < actions.size() && i < SLOT_IDS.length; ++i) {
            Button b = actions.get(i);
            float x = layX(SLOT_IDS[i], SLOT_DEFAULTS[i][0]) * getWidth();
            float y = layY(SLOT_IDS[i], SLOT_DEFAULTS[i][1]) * getHeight();
            b.rect.set(x - r, y - r, x + r, y + r);
            boolean on = ((b.label.equals("Order") || b.label.equals("Приказ")) && rightTaps)
                    || ((b.label.equals("Select") || b.label.equals("Выбор")) && boxArmed);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(alphaOf(on ? 0xd0ffb03a : 0x900a1a2a));
            c.drawOval(b.rect, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(2 * density);
            fill.setColor(alphaOf(on ? 0xffffb03a : 0xff40d6c8));
            c.drawOval(b.rect, fill);
            fill.setStyle(Paint.Style.FILL);
            text.setColor(alphaOf(0xffffffff));
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 6 * density * scale(), text);
        }
        text.setTextSize(14 * density);
        text.setColor(Color.WHITE);
    }

    private void drawPanel(Canvas c) {
        float w = Math.min(getWidth() - 24 * density, 700 * density);
        float row = 36 * density, rowGap = 6 * density, pad = 16 * density, header = 56 * density;
        float section = 26 * density, footer = 62 * density;
        int rows = Math.max(graphics.length, controlsSettings.length);
        // rows a bit lower when the panel would not fit the screen's height (wide phones in landscape)
        float avail = getHeight() - 12 * density - (header + section + footer);
        if (rows * (row + rowGap) > avail && avail > 0) {
            float k = avail / (rows * (row + rowGap));
            row *= k;
            rowGap *= k;
        }
        float h = header + section + rows * (row + rowGap) + footer;
        float left = (getWidth() - w) / 2, top = Math.max(6 * density, (getHeight() - h) / 2);
        panel.set(left, top, left + w, top + h);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0xf20b1626);
        c.drawRoundRect(panel, 20 * density, 20 * density, fill);
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(1.5f * density);
        fill.setColor(0xff2a5a6a);
        c.drawRoundRect(panel, 20 * density, 20 * density, fill);
        fill.setStyle(Paint.Style.FILL);
        // header: title left, the credit under it, a close cross right
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(Color.WHITE);
        text.setTextSize(19 * density);
        c.drawText(ru() ? "Настройки" : "Settings", left + pad, top + 30 * density, text);
        text.setTextSize(11 * density);
        text.setColor(0xff8aa8b8);
        c.drawText(SetupActivity.CREDIT + (ru() ? "   ·   * — после перезапуска" : "   ·   * = after restart"),
                left + pad, top + 47 * density, text);
        float cs = 34 * density;
        panelClose.set(left + w - pad - cs, top + 12 * density, left + w - pad, top + 12 * density + cs);
        fill.setColor(0xff1a2c3c);
        c.drawRoundRect(panelClose, 10 * density, 10 * density, fill);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);
        text.setTextSize(18 * density);
        c.drawText("×", panelClose.centerX(), panelClose.centerY() + 6 * density, text);
        float colW = (w - pad * 3) / 2;
        String[] titles = {ru() ? "ГРАФИКА" : "GRAPHICS", ru() ? "УПРАВЛЕНИЕ" : "CONTROLS"};
        Setting[][] cols = {graphics, controlsSettings};
        for (int col = 0; col < 2; ++col) {
            float cx = left + pad + col * (colW + pad), y = top + header;
            text.setTextAlign(Paint.Align.LEFT);
            text.setTextSize(11 * density);
            text.setColor(TEAL);
            c.drawText(titles[col], cx + 4 * density, y + 14 * density, text);
            y += section;
            for (Setting s : cols[col]) {
                int i = java.util.Arrays.asList(settings).indexOf(s);
                RectF rect = settingRects[i];
                rect.set(cx, y, cx + colW, y + row);
                fill.setColor(0xff142638);
                c.drawRoundRect(rect, 10 * density, 10 * density, fill);
                text.setTextAlign(Paint.Align.LEFT);
                text.setTextSize(13 * density);
                text.setColor(Color.WHITE);
                c.drawText(s.label() + (s.engine ? " *" : ""), rect.left + 12 * density, rect.centerY() + 5 * density,
                        text);
                // ‹ value ›
                float vz = rect.width() * 0.48f, vl = rect.right - vz, mid = rect.right - vz / 2;
                text.setTextAlign(Paint.Align.CENTER);
                text.setColor(0xff6a8a9a);
                text.setTextSize(16 * density);
                c.drawText("‹", vl + 12 * density, rect.centerY() + 6 * density, text);
                c.drawText("›", rect.right - 12 * density, rect.centerY() + 6 * density, text);
                text.setColor(TEAL);
                text.setTextSize(13 * density);
                c.drawText(s.names[s.index], mid, rect.centerY() + 5 * density, text);
                y += row + rowGap;
            }
        }
        // footer: layout editor, support, source code, close
        float by = top + h - footer + 12 * density, bh = 38 * density, bw = (w - pad * 5) / 4;
        panelLayout.set(left + pad, by, left + pad + bw, by + bh);
        panelSupport.set(panelLayout.right + pad, by, panelLayout.right + pad + bw, by + bh);
        panelGitHub.set(panelSupport.right + pad, by, panelSupport.right + pad + bw, by + bh);
        RectF closeBig = new RectF(panelGitHub.right + pad, by, panelGitHub.right + pad + bw, by + bh);
        fill.setColor(0xff1d5a6a);
        c.drawRoundRect(panelLayout, 10 * density, 10 * density, fill);
        fill.setColor(0xff7a3a6e);
        c.drawRoundRect(panelSupport, 10 * density, 10 * density, fill);
        fill.setColor(0xff2d333b);
        c.drawRoundRect(panelGitHub, 10 * density, 10 * density, fill);
        fill.setColor(0xff22384a);
        c.drawRoundRect(closeBig, 10 * density, 10 * density, fill);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);
        text.setTextSize(14 * density);
        c.drawText(ru() ? "Раскладка" : "Layout", panelLayout.centerX(), panelLayout.centerY() + 5 * density, text);
        c.drawText("Patreon", panelSupport.centerX(), panelSupport.centerY() + 5 * density, text);
        c.drawText("GitHub", panelGitHub.centerX(), panelGitHub.centerY() + 5 * density, text);
        c.drawText(ru() ? "Закрыть" : "Close", closeBig.centerX(), closeBig.centerY() + 5 * density, text);
        panelCloseBig.set(closeBig);
    }

    /** The system back button opens the game's menu, like Esc. */
    static boolean onBack(Activity a) {
        key(KeyEvent.KEYCODE_ESCAPE);
        return true;
    }
}
