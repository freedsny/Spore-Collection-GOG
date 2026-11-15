package org.libsdl.app;

/** Typed text for the game (SDL text input, i.e. WM_CHAR): SDLInputConnection is package-private in SDL. */
public final class SporeText {
    private SporeText() {}

    public static void type(String text) {
        SDLInputConnection.nativeCommitText(text, 1);
    }

    /**
     * SDL's own connection to the phone keyboard for a view of the port (SporeControls): it compares what the keyboard
     * holds with what was already sent, so letters, accents, voice typing, word suggestions and backspace all reach the
     * game as SDL text input and key presses.
     */
    public static android.view.inputmethod.InputConnection connection(android.view.View view) {
        return new SDLInputConnection(view, true);
    }
}
