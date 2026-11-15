#!/bin/bash
# Builds APKs of this fork without the game files and without the NDK: the Java code, the manifest and the resources
# come from this repo, the native libraries (libmain.so, SDL2, DXVK, Turnip) are taken unchanged from official release
# APKs of VinTeRuS/spore-re. Only for changes on the Java side (android/java, android/res, the manifest); a change to
# the native code needs the full build (README). The release must match the Java code's JNI functions (the natives
# declared in android/java and implemented in src/native).
#
# The APK gets its own application id (APP_ID) and is signed with this fork's key, so it installs next to the official
# app instead of replacing it. Its saves and settings live in Android/data/<APP_ID>/files.
#
# Usage: android/build-fork-apk.sh official.apk [...]     -> build/fork-apk/<same file name>
# Needs: Android SDK (platform android-35, build-tools 35+), SDL2 sources (SDL_SRC, the tag of android/build-deps.sh)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BUILD_TOOLS=${BUILD_TOOLS:-$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)}
PLATFORM_JAR=${PLATFORM_JAR:-$SDK/platforms/android-35/android.jar}
SDL_SRC=${SDL_SRC:-$ROOT/build/sdl}
KEYSTORE=${KEYSTORE:-$ROOT/android/fork.keystore}
APP_ID=${APP_ID:-com.sporere.game.freed}
APP_LABEL=${APP_LABEL:-Spore · VinTeRuS (Freed)}

[ $# -gt 0 ] || { echo "usage: $0 official.apk [...]" >&2; exit 1; }

OUT=$ROOT/build/fork-apk
WORK=$OUT/work
rm -rf "$WORK"
mkdir -p "$WORK/classes" "$WORK/dex"

# Java: SDL's activity classes plus the port's (as android/build-apk.sh).
javac --release 11 -nowarn -cp "$PLATFORM_JAR" -d "$WORK/classes" \
    "$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java \
    "$ROOT"/android/java/com/sporere/game/*.java "$ROOT"/android/java/org/libsdl/app/*.java
"$BUILD_TOOLS/d8" --release --min-api 26 --lib "$PLATFORM_JAR" --output "$WORK/dex" \
    $(find "$WORK/classes" -name '*.class')

# Manifest and resources: another application id and name on the launcher.
sed "s/android:label=\"Spore · VinTeRuS\"/android:label=\"$APP_LABEL\"/" "$ROOT/android/AndroidManifest.xml" \
    > "$WORK/AndroidManifest.xml"
"$BUILD_TOOLS/aapt2" compile --dir "$ROOT/android/res" -o "$WORK/res.zip"
"$BUILD_TOOLS/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM_JAR" --manifest "$WORK/AndroidManifest.xml" \
    -R "$WORK/res.zip" --auto-add-overlay --min-sdk-version 26 --target-sdk-version 35 \
    --rename-manifest-package "$APP_ID"

for official in "$@"; do
    name=$(basename "$official")
    pkg=$WORK/pkg-$name
    rm -rf "$pkg"
    mkdir -p "$pkg"
    unzip -q "$official" 'lib/*' -d "$pkg"
    cp "$WORK/dex/classes.dex" "$pkg/"
    cp "$WORK/base.apk" "$WORK/unsigned-$name"
    (cd "$pkg" && zip -q -r "$WORK/unsigned-$name" classes.dex lib)
    "$BUILD_TOOLS/zipalign" -P 16 -f 4 "$WORK/unsigned-$name" "$WORK/aligned-$name"
    "$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
        --out "$OUT/$name" "$WORK/aligned-$name"
    ls -la "$OUT/$name"
done
