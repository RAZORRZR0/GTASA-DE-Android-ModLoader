# GTA SA Definitive Edition: no-root Android mod loader

Target: `com.rockstargames.gtasa.de` v1.87.0, arm64-v8a. Not tested on a device
by this repo; see `docs/` for the research and build notes.

This repo contains:

- `loader/`: builds AML (Android Mod Loader, a submodule) for arm64 and injects
  it into the APK (`patch_apk_aml.sh`).
- `mods/`: mods built against AML. `hello` is a test mod. `propfix` is a port
  of GTA_Prop_Fix (PC) to Android. `projectpropsde` is a port of ProjectPropsDE
  (Project Props 4 map props, see `mods/projectpropsde/README.md`). `cleo` is a
  port of CLEO Android 2.0.1 (BSD-3): swipe-down CLEO menu, `.fxt` script text,
  touch opcodes and its 14 SA scripts (`mods/cleo/scripts`) in
  `Android/data/<pkg>/cleo/`. Every script is a menu entry: `.csi` start on
  selection, `.csa` toggle on/off (kept in `cleo/enabled.txt`, "on" ones start
  with each game). Scripts adapted to DE 1.87.0, all taken from `libUE4.so`:
  cheats and jetpack to its arm64 code; cheat names to `CCheat::CheatLabel`
  order (106 cheats); ONMISSION `$408`→`$410` (`CTheScripts::OnAMissionFlag`
  = 1640) in mission selector and fightmaster; widget IDs to DE's
  `CTouchInterface::m_pWidgets` slots (carboost 38→41, passenger 48→55,
  flying 23→26, 30→33). `pakloader` mounts the user's own `.pak` files from
  `Android/data/<pkg>/paks/` above the game's paks (e.g. GTA Radio Restore's
  six PC station paks, which load as-is). `loadingmusic` ports SALoadingMusic
  (PC): one of the eight `S_00N` loading themes plays while a save loads.
- `sdk/`: `gtade.h` (SEH-free safe-read helpers) and `test_host.cpp`.
- `docs/`: research notes, settings table, build notes.

## Build

```sh
git submodule update --init --recursive
loader/build_aml_arm64.sh       # applies the two arm64 patches, builds libAML.so
mods/build.sh                   # builds every mod
loader/patch_apk_aml.sh <base.apk> loader/aml/libs/arm64-v8a/libAML.so <out.apk>  # PC install only
```

The NDK path defaults to `~/Library/Android/sdk/ndk/27.3.13750724`. Override
with `NDK_BUILD=/path/to/ndk-build` or `ANDROID_NDK_HOME=/path/to/ndk`.

`patch_apk_aml.sh` generates `loader/debug.keystore` on first run. The
keystore is git-ignored and is never committed.

## Install

On the phone, with [Morphe](https://morphe.software): add the patch source
`https://github.com/RAZORRZR0/gtasa-de-morphe-patches` in Morphe Manager, select
GTA: SA 1.87.0 and the mods, patch and install. The patches bundle AML and the
mods built here (their repo's `sync-mods.sh` copies them).

From a PC instead:

1. Back up your saves, uninstall the original (this wipes its data), then
   install the patched APK (`loader/patch_apk_aml.sh`).
2. Copy each mod's `libs/arm64-v8a/lib<mod>.so` to
   `Android/data/com.rockstargames.gtasa.de/mods/` and its `.ini`/data files to
   `Android/data/com.rockstargames.gtasa.de/configs/`. The defaults are in
   `docs/settings-defaults.md`.

## Credits and licenses

- AML (`loader/aml`): RusJJ, MIT. Pinned to `3ef5c918ec855f3db5e3a216dcb176e209b89922`.
  ARMPatch (nested in AML) is pinned to `62f670228d712286d368c7d73cd9835e371ee979`.
- GTA_Prop_Fix (PC): RAZORRZR0, MIT. Source: https://github.com/RAZORRZR0/GTA_Prop_Fix
- This repo: MIT, see `LICENSE`.
