# spore-re

[Español](README.md) | English | [Русский](README.ru.md)

Native Android port of Spore (with Galactic Adventures). Not an emulator and not Wine: `SporeApp.exe` is statically
recompiled from x86 to C, built for arm64 and run on top of my own Win32/D3D9 layer (SDL2, DXVK).

> **The project is at a very early stage.** Expect a lot of small, frequent commits, quick fixes, things breaking
> and getting rewritten.

If you like the port, you can support me on Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)**.

## Download

Test builds (APK) are on the **[Releases](https://github.com/VinTeRuS/spore-re/releases)** page. The APK has no game
files in it: you need your own copy of Spore (see below).

## Installing

Android 10+, arm64 (phones from about 2019 on). Only tested on a Pixel 6 so far. Two APKs: `spore.apk` for GPUs with
Vulkan 1.3 (most phones), `spore-vk11.apk` for older GPUs whose driver only has Vulkan 1.1 (older Mali, PowerVR).

1. Copy the game folders `Data`, `DataEP1`, `SporebinEP1` and `bp1content` from your Spore install to
   `Internal storage/Spore` on the phone (so there is `/sdcard/Spore/SporebinEP1/SporeApp.exe`).
2. Install the APK, start it and allow "All files access".
3. The game follows the phone's language; Settings (round button -> gear) -> Language picks another one.

## Game version

GOG Spore Collection 3.1.0.22, `SporebinEP1/SporeApp.exe` (sha256 in `re/target.sha256`). Other builds won't work.

## Status

- All stages (cell, creature, tribe, civilization, space), the editors, saves and sound work on a Pixel 6.
- Touch controls change per stage (with a short hint): tap = click, hold = right click, one finger turns the
  camera (creature, space) or moves the map (tribe, civilization), two fingers turn, pinch zooms, a movement stick and
  ability buttons in the creature stage. Every button can be moved (Settings -> Layout).
- Settings: resolution, sharpness (an upscaling filter, so a lower resolution keeps the interface big and crisp),
  FPS limit 30-120, quality, button size and opacity.
- Known problems: no Space Adventures entry in the main menu yet, C++ exceptions thrown by the game aren't handled
  (some mod-made profiles crash), "Quit" in the pause menu closes the app (the game's own exit).

## What's in the repo

My own code: the Win32/D3D9 host, the Android app (Java, touch controls, settings), build scripts, the recompiler
(`third_party/x86recomp`) and patches for DXVK. No game files and no recompiled game code: that code is generated
from your own copy of the game at build time.

## Building (rough)

Linux, Python 3, Android NDK, the game installed somewhere.

1. `python3 -I third_party/x86recomp/tools/recomp.py --functions all --split 400 --out build/recomp/all` generates
   C from the game (settings in `tools/recomp/game.py`).
2. `make -j3` in `src/native` for a desktop (Linux) build, `make -j3 PLATFORM=android` for arm64.
3. `android/build-deps.sh` (SDL2, FAudio, DXVK with the patches from `android/patches`), then `android/build-apk.sh`
   gives `build/apk/spore.apk`.
4. Optional profile-guided optimization: `make PLATFORM=android PGO=gen OUT=../../build/android-pgo-gen/arm64-v8a`,
   play that build for a while, then `android/pgo.sh` builds the optimized APK.

## Legal

You need your own bought copy of Spore. Spore is a trademark of Electronic Arts; this project isn't affiliated with
EA or Maxis.
