package com.sporere.game;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Launcher: finds the game files and starts SporeActivity, or explains where to put them. The game can live in the
 * app's own folder (Android/data/com.sporere.game/files/game, which file managers often cannot open on Android 11+)
 * or in /sdcard/Spore, which needs "all files access" (storage permission before Android 11).
 */
public class SetupActivity extends Activity {
    /** Shown on the setup screen and in the in-game settings panel. */
    // The native library checks CREDIT (SporeActivity hands it over): a build without it does not start.
    static final String CREDIT = "Spore Android port by VinTeRuS";
    static final String CREDIT_URL = "patreon.com/cw/VinTeRuS";
    static final String SUPPORT_URL = "https://www.patreon.com/cw/VinTeRuS/membership";

    static final String GITHUB_URL = "https://github.com/VinTeRuS/spore-re";

    /** Opens the author's Patreon page in the browser. */
    static void openSupport(android.content.Context context) {
        openUrl(context, SUPPORT_URL);
    }

    /** Opens the port's source code (GitHub) in the browser. */
    static void openGitHub(android.content.Context context) {
        openUrl(context, GITHUB_URL);
    }

    private static void openUrl(android.content.Context context, String url) {
        try {
            context.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception ignored) {
            // no browser
        }
    }

    private static final String EXE = "SporebinEP1/SporeApp.exe";
    private static final int REQUEST_STORAGE = 1;

    private File appGame() {
        File base = getExternalFilesDir(null);  // also creates Android/data/<package>/files
        return base == null ? null : new File(base, "game");
    }

    private static File sharedGame() {
        return new File(Environment.getExternalStorageDirectory(), "Spore");
    }

    private boolean hasSharedAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected void onResume() {
        super.onResume();
        check();  // also after coming back from the permission screen
    }

    /**
     * Files the game needs from the PC game folder (GOG's SPORE folder with Galactic Adventures): the program and the
     * main archives of the base game and the expansion.
     */
    private static final String[] REQUIRED = {
        EXE, "Data/Spore_Game.package", "Data/Spore_Graphics.package", "Data/Spore_Content.package",
        "DataEP1/Spore_EP1_Data.package",
    };

    /** `rel` under `dir`, each path component matched ignoring case (as the game does on Windows); null if absent. */
    private static File find(File dir, String rel) {
        File cur = dir;
        for (String part : rel.split("/")) {
            String[] names = cur == null ? null : cur.list();
            File next = null;
            if (names != null) {
                for (String n : names) {
                    if (n.equalsIgnoreCase(part)) {
                        next = new File(cur, n);
                        break;
                    }
                }
            }
            if (next == null) return null;
            cur = next;
        }
        return cur;
    }

    /** The required files missing from `dir`. */
    private static java.util.List<String> missing(File dir) {
        java.util.List<String> out = new java.util.ArrayList<>();
        for (String r : REQUIRED) if (find(dir, r) == null) out.add(r);
        return out;
    }

    private void check() {
        File app = appGame();
        if (app != null) app.mkdirs();
        File shared = sharedGame();
        if (hasSharedAccess()) shared.mkdirs();
        File found = null;
        if (app != null && find(app, EXE) != null) found = app;
        else if (hasSharedAccess() && find(shared, EXE) != null) found = shared;
        else if (hasSharedAccess()) {
            // the GOG folder copied whole into Spore/ (Spore/SPORE/Data/...): one level down counts too
            File[] subs = shared.listFiles(File::isDirectory);
            if (subs != null) {
                for (File sub : subs) {
                    if (find(sub, EXE) != null) {
                        found = sub;
                        break;
                    }
                }
            }
        }
        if (found == null) {
            showHelp(app, shared, null);
            return;
        }
        java.util.List<String> lack = missing(found);
        if (!lack.isEmpty()) {
            showHelp(app, shared, found.getAbsolutePath() + " is missing: " + String.join(", ", lack));
            return;
        }
        launch(found == app ? null : found);
    }

    private void launch(File gameDir) {
        try {
            if (gameDir == null) Os.unsetenv("SPORERE_GAME");
            else Os.setenv("SPORERE_GAME", gameDir.getAbsolutePath(), true);
        } catch (ErrnoException ignored) {
            // the native side falls back to the app folder
        }
        copyLogs(gameDir);
        File driver = installCustomDriver(gameDir);
        try {
            if (driver == null) Os.unsetenv("SPORERE_CUSTOM_DRIVER");
            else Os.setenv("SPORERE_CUSTOM_DRIVER", driver.getAbsolutePath(), true);
        } catch (ErrnoException ignored) {
            // the bundled drivers are used
        }
        startActivity(new Intent(this, SporeActivity.class));
        finish();
    }

    /**
     * A user-supplied Vulkan driver (an Adreno driver, e.g. a Turnip build made for emulators): a .so, or a .zip as
     * Adreno Tools driver releases ship them, in Spore/driver/ next to the game files (or the older place,
     * Android/data/com.sporere.game/files/driver/). Copied into the app's private storage, where the linker may load
     * it from (not from shared storage). Returns the private copy, or null when there is none (the copy is removed
     * when the user removes the driver).
     */
    private File installCustomDriver(File gameDir) {
        File base = getExternalFilesDir(null);
        File[] dirs = {gameDir == null ? null : new File(gameDir, "driver"), base == null ? null : new File(base, "driver")};
        File src = null;
        for (File dir : dirs) {
            File[] found = dir == null ? null
                    : dir.listFiles((d, name) -> name.endsWith(".so") || name.endsWith(".zip"));
            if (found != null && found.length > 0) {
                src = found[0];
                break;
            }
        }
        File dest = new File(new File(getFilesDir(), "driver"), "libvulkan_custom.so");
        File stamp = new File(dest.getParentFile(), "source");
        if (src == null) {
            dest.delete();
            stamp.delete();
            return null;
        }
        String id = src.getAbsolutePath() + " " + src.length() + " " + src.lastModified();
        if (dest.exists() && id.equals(readText(stamp))) return dest;
        dest.getParentFile().mkdirs();
        try (InputStream raw = new FileInputStream(src); OutputStream out = new FileOutputStream(dest)) {
            InputStream in = raw;
            if (src.getName().endsWith(".zip")) {  // the first .so inside (Adreno Tools: the driver + meta.json)
                java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(raw);
                java.util.zip.ZipEntry e;
                while ((e = zip.getNextEntry()) != null && !(e.getName().endsWith(".so") && !e.isDirectory())) {}
                if (e == null) throw new IOException("no .so in " + src.getName());
                in = zip;
            }
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        } catch (IOException e) {
            dest.delete();
            return null;
        }
        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(id.getBytes());
        } catch (IOException ignored) {
            // copied again next time
        }
        return dest;
    }

    /**
     * The last run's logs into Spore/logs/ next to the game files, where any file manager reaches them (the app's
     * own folder under Android/data is closed to them on newer Android): for bug reports.
     */
    private void copyLogs(File gameDir) {
        File base = getExternalFilesDir(null);
        if (gameDir == null || base == null) return;
        File out = new File(gameDir, "logs");
        String[] names = {"spore-native.log", "spore-native.prev.log", "app_process64_d3d9.log"};
        for (String name : names) {
            File src = new File(base, name);
            if (!src.isFile()) continue;
            out.mkdirs();
            try (InputStream in = new FileInputStream(src); OutputStream o = new FileOutputStream(new File(out, name))) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) o.write(buf, 0, n);
            } catch (IOException ignored) {
                // a log is a nice-to-have
            }
        }
    }

    private static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 4096)];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n) : "";
        } catch (IOException e) {
            return "";
        }
    }

    private void requestAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        check();
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(String s, float sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(Color.rgb(230, 230, 230));
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, 0, 0, dp(12));
        return t;
    }

    private Button button(String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, dp(12), 0);
        b.setLayoutParams(lp);
        return b;
    }

    private void showHelp(File app, File shared, String problem) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(32), dp(24), dp(32), dp(24));
        col.addView(text(SporeControls.tx(problem == null ? "Game files not found" : "Some game files are missing"), 22, true));
        if (problem != null) {
            TextView p = text(problem, 15, true);
            p.setTextColor(Color.rgb(255, 120, 100));
            col.addView(p);
        }
        col.addView(text(SporeControls.tx("Copy these folders from your SPORE folder on the PC (GOG version, with Galactic "
                + "Adventures): SporebinEP1, Data, DataEP1 and bp1content (about 6 GB), to one of these places:"),
                15, false));
        col.addView(text("1. " + (app != null ? app.getAbsolutePath() : "Android/data/" + getPackageName()
                + "/files/game") + SporeControls.tx("\n   The folder exists now. On Android 11+ most file managers cannot open "
                + "Android/data; copy from a PC over USB, or use option 2."), 14, false));
        String access = SporeControls.tx(hasSharedAccess() ? "access granted" : "needs file access: tap the button below");
        col.addView(text("2. " + shared.getAbsolutePath() + SporeControls.tx("\n   Any file manager can open it (") + access + ").",
                14, false));
        col.addView(text(SporeControls.tx("Then tap Check again."), 15, false));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.START);
        row.addView(button(SporeControls.tx("Check again"), this::check));
        if (!hasSharedAccess()) row.addView(button(SporeControls.tx("Allow file access"), this::requestAccess));
        row.addView(button(SporeControls.tx("Exit"), this::finish));
        row.addView(button(SporeControls.tx("Support on Patreon"), () -> openSupport(this)));
        row.addView(button("GitHub", () -> openGitHub(this)));
        col.addView(row);
        TextView credit = text(CREDIT + " \u00b7 " + CREDIT_URL, 12, false);
        credit.setTextColor(Color.rgb(150, 150, 150));
        credit.setPadding(0, dp(24), 0, 0);
        col.addView(credit);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(18, 18, 18));
        scroll.addView(col);
        setContentView(scroll);
    }
}
