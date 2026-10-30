#!/bin/sh
# Build every mod in mods/ for arm64-v8a.
# Output: mods/<name>/libs/arm64-v8a/lib<name>.so
# Env overrides: NDK_BUILD (path to ndk-build) or ANDROID_NDK_HOME.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
NDK_BUILD="${NDK_BUILD:-${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.3.13750724}/ndk-build}"
[ -x "$NDK_BUILD" ] || { echo "ndk-build not found at $NDK_BUILD (set NDK_BUILD or ANDROID_NDK_HOME)" >&2; exit 1; }

for mod in hello propfix uidump modmenu; do
    echo "== $mod"
    (cd "$HERE/$mod" && "$NDK_BUILD" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
        APP_PLATFORM=android-23 APP_ABI=arm64-v8a APP_STL=c++_static -j8)
done
