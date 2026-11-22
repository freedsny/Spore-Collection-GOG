#!/bin/bash
# The build for GPUs whose driver only has Vulkan 1.1 (older Mali, PowerVR): DXVK 2.x needs Vulkan 1.3, so this APK
# carries dxvk-native 1.9.2 (Joshua Ashton's native fork of DXVK 1.9.2) with android/patches/dxvk19-native-android.patch
# (arm64, Android surface, optional GPU features, no swap chain recreation on SUBOPTIMAL) instead.
#   android/build-vk11.sh          -> build/apk/spore-vk11.apk
# Needs the normal Android build first (build/android/arm64-v8a: its objects are reused, only vulkan_driver.cpp is
# rebuilt with SPORERE_VK11) and the deps from android/build-deps.sh (SDL2).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
DEPS=${DEPS_ROOT:-$HOME/spore-re-data/android-deps}/arm64-v8a
VK11=${VK11_ROOT:-$HOME/spore-re-data/android-deps-vk11}
SRC=${VK11_SRC:-$HOME/spore-re-data/dxvk19-src}
TAG=native-1.9.2b
PGO=${PGO:-}  # the profile the normal build used, if any (android/pgo.sh: build/pgo/spore.profdata)

mkdir -p "$SRC" "$VK11/arm64-v8a/lib"
if [ ! -d "$SRC/dxvk-native-$TAG" ]; then
    curl -sL -o "$SRC/dxvk-native.tar.gz" "https://github.com/Joshua-Ashton/dxvk-native/archive/refs/tags/$TAG.tar.gz"
    tar -xzf "$SRC/dxvk-native.tar.gz" -C "$SRC"
    patch -d "$SRC/dxvk-native-$TAG" -p1 < "$ROOT/android/patches/dxvk19-native-android.patch"
fi
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cat > "$SRC/android-arm64-v8a.cross" <<EOF
[binaries]
c = '$TC/aarch64-linux-android26-clang'
cpp = '$TC/aarch64-linux-android26-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$DEPS/lib/pkgconfig'
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF
rm -rf "$SRC/build19"
meson setup "$SRC/build19" "$SRC/dxvk-native-$TAG" --cross-file "$SRC/android-arm64-v8a.cross" --buildtype release \
    --prefix "$VK11/arm64-v8a" -Ddxvk_native_force=true -Ddxvk_native_wsi=sdl2 -Denable_dxgi=false \
    -Denable_d3d10=false -Denable_d3d11=false \
    -Dc_args="-I$DEPS/include -I$DEPS/include/SDL2" -Dcpp_args="-I$DEPS/include -I$DEPS/include/SDL2" \
    -Dc_link_args="-L$DEPS/lib -static-libstdc++" -Dcpp_link_args="-L$DEPS/lib -static-libstdc++"
ninja -C "$SRC/build19" -j"${JOBS:-3}"
ninja -C "$SRC/build19" install
cp "$DEPS/lib/libSDL2.so" "$VK11/arm64-v8a/lib/"

# libmain.so: the normal build's objects, vulkan_driver.cpp again with SPORERE_VK11 (Vulkan 1.1 is enough)
rm -rf "$ROOT/build/android-vk11"
mkdir -p "$ROOT/build/android-vk11"
cp -a "$ROOT/build/android/arm64-v8a" "$ROOT/build/android-vk11/"
rm -f "$ROOT/build/android-vk11/arm64-v8a/vulkan_driver.o" "$ROOT/build/android-vk11/arm64-v8a/libmain.so"
make -C "$ROOT/src/native" -j"${JOBS:-3}" PLATFORM=android SPORERE_VK11=1 ${PGO:+PGO=$PGO} \
    OUT=../../build/android-vk11/arm64-v8a
# and the ARMv8.0 one for older cores (the phones that most often have only Vulkan 1.1)
cp -a "$ROOT/build/android/arm64-v8a-v80" "$ROOT/build/android-vk11/"
rm -f "$ROOT/build/android-vk11/arm64-v8a-v80/vulkan_driver.o" "$ROOT/build/android-vk11/arm64-v8a-v80/libmain.so"
make -C "$ROOT/src/native" -j"${JOBS:-3}" PLATFORM=android SPORERE_VK11=1 ARM_ARCH=armv8-a ${PGO:+PGO=$PGO} \
    OUT=../../build/android-vk11/arm64-v8a-v80
DEPS_ROOT=$VK11 ANDROID_BUILD="$ROOT/build/android-vk11" APK_NAME=spore-vk11.apk "$ROOT/android/build-apk.sh"
