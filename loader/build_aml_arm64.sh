#!/bin/sh
# Build AML (libAML.so) for arm64-v8a only.
# Env overrides: NDK_BUILD (path to ndk-build) or ANDROID_NDK_HOME.
set -e

AML="$(cd "$(dirname "$0")" && pwd)/aml"
NDK_BUILD="${NDK_BUILD:-${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.3.13750724}/ndk-build}"
COMMON="APP_ABI=arm64-v8a APP_STL=c++_static APP_PLATFORM=android-21"
ARMP="$AML/ARMPatch/armpatch_src"
LOADER="$(cd "$(dirname "$0")" && pwd)"

# 0. arm64 build fixes against the pinned submodules (idempotent).
apply_once() { # <repo dir> <patch file>
    if git -C "$1" apply --check -R "$2" 2>/dev/null; then
        echo "already applied: $2"
    else
        git -C "$1" apply "$2"
    fi
}
apply_once "$AML/ARMPatch" "$LOADER/armpatch-arm64.patch"
apply_once "$AML" "$LOADER/aml-arm64.patch"

# 1. ARMPatch static library (needed by AML). ndk-build wants <project>/jni/Android.mk.
rm -rf "$AML/.armpatch_proj"
mkdir -p "$AML/.armpatch_proj"
ln -s "$ARMP" "$AML/.armpatch_proj/jni"
NDK_PROJECT_PATH="$AML/.armpatch_proj" \
NDK_OUT="$ARMP/obj" NDK_LIBS_OUT="$ARMP/libs" \
"$NDK_BUILD" $COMMON -j8
rm -rf "$AML/.armpatch_proj"

mkdir -p "$AML/obj/local/arm64-v8a"
cp "$ARMP/obj/local/arm64-v8a/libarmpatch.a" "$AML/obj/local/arm64-v8a/"

# 2. AML itself
rm -rf "$AML/.aml_proj"
mkdir -p "$AML/.aml_proj"
ln -s "$AML" "$AML/.aml_proj/jni"
NDK_PROJECT_PATH="$AML/.aml_proj" \
NDK_OUT="$AML/obj" NDK_LIBS_OUT="$AML/libs" \
"$NDK_BUILD" $COMMON -j8
rm -rf "$AML/.aml_proj"

file "$AML/libs/arm64-v8a/libAML.so"
