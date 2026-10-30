# modmenu

Adds a **mods** entry to the pause menu (and main menu), placed before *exit*. The goal is a native-looking screen where each mod can be turned on/off and its ini edited.

## Status
Working on device (main + pause menu): mod list with on/off, per-mod settings page (Bool/Choice keys editable,
numbers read-only). Design and internals: `docs/mods_menu_design.md`; Blueprint RE: `docs/bp_decompiled.md`.

## Build / test
```
~/Library/Android/sdk/ndk/27.3.13750724/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-23 APP_ABI=arm64-v8a APP_STL=c++_static
c++ -std=c++17 -Wall -o /tmp/test_core test_core.cpp modmenu_core.cpp && /tmp/test_core
```
Deploy `libs/arm64-v8a/libmodmenu.so` to `mods/` and `configs/modmenu.ini` to `configs/`. Then run `adb logcat -s ModMenu`.

Expected log: `installed`, then `names resolved`, then on opening the pause menu `GTASA_PauseMenu_Mobile: inserted 'mods' at 7 (tab class copied from index 5)`.
