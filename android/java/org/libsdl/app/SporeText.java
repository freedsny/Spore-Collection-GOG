package org.libsdl.app;

import android.view.View;
import android.view.inputmethod.InputConnection;

/** Typed text for the game (SDL text input, i.e. WM_CHAR): SDLInputConnection is package-private in SDL. */
public final class SporeText {
    private SporeText() {}

    public static void type(String text) {
        SDLInputConnection.nativeCommitText(text, 1);
    }

    /** The on-screen keyboard's link to the game for a view that has the focus (letters, backspace, enter). */
    public static InputConnection connect(View view) {
        return new SDLInputConnection(view, true);
    }
}
