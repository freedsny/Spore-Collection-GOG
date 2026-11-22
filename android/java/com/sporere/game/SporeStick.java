package com.sporere.game;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

/**
 * A movement stick for the stages that walk (or pan the camera) with W/A/S/D: a ring with a knob that holds the keys
 * of its direction (diagonals hold two), and a jump button (Space). A view of its own, so a finger on the stick and
 * another one on the game are separate touch streams. Where both sit, how big and how opaque they are comes from
 * the layout the player set (SporeControls' layout editor and settings: prefs "lay_stick_*", "lay_jump_*",
 * "ui_scale", "ui_alpha").
 */
public class SporeStick extends View {
    private static final int TEAL = 0x40d6c8;
    private static final int[] WASD = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};
    // the space stage flies with the arrow keys (the game's own tip: "Use the arrow keys to move your spaceship")
    private static final int[] ARROWS = {KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_LEFT,
                                         KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT};
    private int[] keys = WASD;

    /** Arrow keys instead of W/A/S/D (space stage); whatever is held is released first. */
    void useArrows(boolean arrows) {
        int[] k = arrows ? ARROWS : WASD;
        if (k == keys) return;
        setKeys(false, false, false, false);
        keys = k;
    }
    // default places, as fractions of the screen
    static final float STICK_X = 0.13f, STICK_Y = 0.70f, JUMP_X = 0.79f, JUMP_Y = 0.84f;

    /** While the layout editor runs, the stick takes no touches (the editor moves it). */
    static boolean editing;

    private final float density;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final SharedPreferences prefs;
    private final boolean[] held = new boolean[4];  // W A S D
    private float cx, cy, radius, knobX, knobY, jumpX, jumpY, jumpR;
    private int alpha = 255;
    private int stickPointer = -1, jumpPointer = -1;
    /** The controls below: fingers that land elsewhere while the stick is held go there (the stick owns the gesture). */
    View under;

    public SporeStick(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE);
        setVisibility(GONE);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        applyLayout();
    }

    /** Re-reads the place, size and opacity from the saved layout. */
    void applyLayout() {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;
        float scale = prefs.getInt("ui_scale", 100) / 100f;
        alpha = Math.round(prefs.getInt("ui_alpha", 80) / 100f * 255);
        radius = 62 * density * scale;
        jumpR = 36 * density * scale;
        cx = prefs.getFloat("lay_stick_x", STICK_X) * w;
        cy = prefs.getFloat("lay_stick_y", STICK_Y) * h;
        jumpX = prefs.getFloat("lay_jump_x", JUMP_X) * w;
        jumpY = prefs.getFloat("lay_jump_y", JUMP_Y) * h;
        knobX = cx;
        knobY = cy;
        invalidate();
    }

    /** Only the stick and jump areas take touches; everything else falls through to the controls below. */
    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (editing) return false;
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && !inStick(e.getX(), e.getY()) && !inJump(e.getX(), e.getY()))
            return false;
        return super.dispatchTouchEvent(e);
    }

    /** Passes the fingers that are not on the stick or jump on to the controls, as an event of their own. */
    private void forward(MotionEvent e) {
        if (under == null) return;
        int action = e.getActionMasked(), index = e.getActionIndex(), n = 0, mine = -1;
        MotionEvent.PointerProperties[] pp = new MotionEvent.PointerProperties[e.getPointerCount()];
        MotionEvent.PointerCoords[] pc = new MotionEvent.PointerCoords[pp.length];
        for (int i = 0; i < pp.length; ++i) {
            int id = e.getPointerId(i);
            if (id == stickPointer || id == jumpPointer) continue;
            if (i == index) mine = n;
            pp[n] = new MotionEvent.PointerProperties();
            e.getPointerProperties(i, pp[n]);
            pc[n] = new MotionEvent.PointerCoords();
            e.getPointerCoords(i, pc[n++]);
        }
        if (n == 0) return;
        int a = action;
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            if (mine < 0) return;
            a = n == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | mine << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            if (mine < 0) return;
            a = n == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | mine << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        }
        MotionEvent f = MotionEvent.obtain(e.getDownTime(), e.getEventTime(), a, n, pp, pc, e.getMetaState(),
                e.getButtonState(), 1, 1, e.getDeviceId(), 0, e.getSource(), e.getFlags());
        under.onTouchEvent(f);
        f.recycle();
    }

    private boolean inStick(float x, float y) {
        return Math.hypot(x - cx, y - cy) < radius * 1.4f;
    }

    private boolean inJump(float x, float y) {
        return Math.hypot(x - jumpX, y - jumpY) < jumpR * 1.3f;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked(), index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(index), y = e.getY(index);
                if (stickPointer < 0 && inStick(x, y)) {
                    stickPointer = e.getPointerId(index);
                    moveKnob(x, y);
                } else if (jumpPointer < 0 && inJump(x, y)) {
                    jumpPointer = e.getPointerId(index);
                    SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SPACE);
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    invalidate();
                }
                forward(e);
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                forward(e);
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    if (e.getPointerId(i) == stickPointer) moveKnob(e.getX(i), e.getY(i));
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = action == MotionEvent.ACTION_CANCEL ? -2 : e.getPointerId(index);
                forward(e);
                if (id == stickPointer || id == -2) {
                    stickPointer = -1;
                    knobX = cx;
                    knobY = cy;
                    setKeys(false, false, false, false);
                }
                if (id == jumpPointer || id == -2) {
                    if (jumpPointer >= 0) SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SPACE);
                    jumpPointer = -1;
                }
                invalidate();
                return true;
            }
            default:
                return true;
        }
    }

    private void moveKnob(float x, float y) {
        float dx = x - cx, dy = y - cy, d = (float) Math.hypot(dx, dy);
        if (d > radius) {
            dx *= radius / d;
            dy *= radius / d;
        }
        knobX = cx + dx;
        knobY = cy + dy;
        float dead = radius * 0.25f;
        if (d < dead) {
            setKeys(false, false, false, false);
        } else {
            // eight directions: a key is held when the stick leans more than 22.5 degrees toward it
            double a = Math.atan2(-dy, dx);  // 0 = right, pi/2 = up
            double s = Math.sin(a), c = Math.cos(a), t = Math.sin(Math.toRadians(22.5));
            setKeys(s > t, c < -t, s < -t, c > t);
        }
        invalidate();
    }

    private void setKeys(boolean w, boolean a, boolean s, boolean d) {
        boolean[] want = {w, a, s, d};
        for (int i = 0; i < 4; ++i) {
            if (want[i] == held[i]) continue;
            held[i] = want[i];
            if (want[i]) SDLActivity.onNativeKeyDown(keys[i]);
            else SDLActivity.onNativeKeyUp(keys[i]);
        }
    }

    /** Hides the stick, releasing whatever it holds. */
    void hide() {
        setKeys(false, false, false, false);
        if (jumpPointer >= 0) SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SPACE);
        stickPointer = jumpPointer = -1;
        setVisibility(GONE);
    }

    private int argb(int a, int rgb) {
        return (Math.min(255, a * alpha / 255) << 24) | rgb;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (editing) return;  // the editor draws the stick's outline itself
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(argb(0x50, 0x0a1a2a));
        c.drawCircle(cx, cy, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2 * density);
        paint.setColor(argb(0xa0, TEAL));
        c.drawCircle(cx, cy, radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(argb(stickPointer >= 0 ? 0xd0 : 0x90, TEAL));
        c.drawCircle(knobX, knobY, radius * 0.42f, paint);
        paint.setColor(argb(jumpPointer >= 0 ? 0xd0 : 0x70, jumpPointer >= 0 ? TEAL : 0x0a1a2a));
        c.drawCircle(jumpX, jumpY, jumpR, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(argb(0xff, TEAL));
        c.drawCircle(jumpX, jumpY, jumpR, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(argb(0xff, 0xffffff));
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(14 * density * radius / (62 * density));
        c.drawText(SporeControls.ru() ? "Прыжок" : SporeControls.tx("Jump"), jumpX, jumpY + 5 * density, paint);
    }
}
