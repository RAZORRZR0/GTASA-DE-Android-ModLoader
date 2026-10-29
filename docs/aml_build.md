# AML build for GTA SA Definitive Edition (arm64-v8a)

Target: `com.rockstargames.gtasa.de` v1.87.0, arm64-v8a. Not tested on a device.

## Artifacts
- `loader/aml/libs/arm64-v8a/libAML.so`: AML core, aarch64. NEEDED: liblog, libdl, libandroid, libc, libm. Exports `GetInterface`, `JNI_OnLoad`.
- `mods/hello/libs/arm64-v8a/libhello.so`: test mod, aarch64. Built from `mods/hello/main.cpp` and `Android.mk`.
- `apk/out/gtasa_aml.apk`: input APK with `libAML.so` bundled and `GameActivity` patched. Signed with the debug keystore.

## Build
- `sh loader/build_aml_arm64.sh` builds ARMPatch, then AML. Exit 0 from a clean state.
- ndk-build needs `<project>/jni/`, so both steps use a temporary project dir with a `jni` symlink. The script removes it afterwards.
- AML's `Android.mk` drops `-mfloat-abi=softfp` and `-mthumb` on arm64, since those are ARM32-only. The same change is in `ARMPatch/armpatch_src/Android.mk`.
- Link needs `APP_STL=c++_static` and `APP_PLATFORM=android-21`.

## APK injection
- `loader/patch_apk_aml.sh <base.apk> <libAML.so> <out.apk>`. It is a copy of `patch_apk.sh` with `gtaml` replaced by `AML`.
- It inserts `System.loadLibrary("AML")` after `System.loadLibrary("UE4")` in `GameActivity.<clinit>`. The DEX is `classes.dex`.
- It bundles `lib/arm64-v8a/libAML.so` and re-signs with `debug.keystore`.

## Device paths
- Mods: `/sdcard/Android/data/com.rockstargames.gtasa.de/mods/`. Put `libhello.so` here.
- Fallback: `/sdcard/AMLMods/com.rockstargames.gtasa.de/`.
- Config is kept by the AML config system under the same mod tree.

## Logcat tags
- AML core: `AndroidModLoader`.
- Hello mod: `Hello Mod`.

Filter with `adb logcat -s AndroidModLoader "Hello Mod"`.

## Hello mod
- `MYMOD` / `NEEDGAME(com.rockstargames.gtasa.de)`.
- `OnModLoad` gets `libUE4.so` with `aml->GetLibHandle`. It logs `GUObjectArray` and `_ZN7CObject11SetIsStaticEb` with `aml->GetSym`.
- It hooks `SetIsStatic(void* self, bool)` with `HOOK`. The hook logs the call and calls the original.

## Checks for the main agent
- `file loader/aml/libs/arm64-v8a/libAML.so` shows aarch64.
- `apksigner verify --verbose apk/out/gtasa_aml.apk` verifies with v3.
- `unzip -l apk/out/gtasa_aml.apk | grep libAML` shows the bundled library.
- On device: copy `libhello.so` to the mods path, install the APK, launch, and check logcat for `AndroidModLoader` and `Hello Mod`.
