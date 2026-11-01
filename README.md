# GTA SA Definitive Edition: no-root Android mod loader

Target: `com.rockstargames.gtasa.de` v1.87.0, arm64-v8a. Not tested on a device
by this repo; see `docs/` for the research and build notes.

This repo contains:

- `loader/`: builds AML (Android Mod Loader, a submodule) for arm64 and injects
  it into the APK (`patch_apk_aml.sh`).
- `mods/`: mods built against AML. `hello` is a test mod. `propfix` is a port
  of GTA_Prop_Fix (PC) to Android. `projectpropsde` is a port of ProjectPropsDE
  (Project Props 4 map props, see `mods/projectpropsde/README.md`).
- `sdk/`: `gtade.h` (SEH-free safe-read helpers) and `test_host.cpp`.
- `docs/`: research notes, settings table, build notes.

## Build

```sh
git submodule update --init --recursive
loader/build_aml_arm64.sh       # applies the two arm64 patches, builds libAML.so
mods/build.sh                   # builds mods/hello and mods/propfix
loader/patch_apk_aml.sh <base.apk> loader/aml/libs/arm64-v8a/libAML.so <out.apk>
```

The NDK path defaults to `~/Library/Android/sdk/ndk/27.3.13750724`. Override
with `NDK_BUILD=/path/to/ndk-build` or `ANDROID_NDK_HOME=/path/to/ndk`.

`patch_apk_aml.sh` generates `loader/debug.keystore` on first run. The
keystore is git-ignored and is never committed.

## Install

1. Back up your saves, uninstall the original (this wipes its data), then
   install the patched APK (`loader/patch_apk_aml.sh`).
2. Copy `mods/propfix/libs/arm64-v8a/libpropfix.so` (built by `mods/build.sh`)
   to `Android/data/com.rockstargames.gtasa.de/mods/`.
3. Copy `mods/propfix/GTA_Prop_Fix.ini` to
   `Android/data/com.rockstargames.gtasa.de/configs/` (verified on device:
   logcat `GTAPropFix: ini .../configs/GTA_Prop_Fix.ini (found)`).
   The defaults are in `docs/settings-defaults.md`.

## Credits and licenses

- AML (`loader/aml`): RusJJ, MIT. Pinned to `3ef5c918ec855f3db5e3a216dcb176e209b89922`.
  ARMPatch (nested in AML) is pinned to `62f670228d712286d368c7d73cd9835e371ee979`.
- GTA_Prop_Fix (PC): RAZORRZR0, MIT. Source: https://github.com/RAZORRZR0/GTA_Prop_Fix
- This repo: MIT, see `LICENSE`.
