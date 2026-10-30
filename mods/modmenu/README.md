# modmenu

Adds a **mods** entry to the pause menu (and main menu), placed before *exit*. The goal is a native-looking screen where each mod can be turned on/off and its ini edited.

## Status
- **Core (`modmenu_core.*`)**: done and host-tested.
  - `ModList` turns a mod on/off by renaming `x.so` ↔ `x.so.disabled`. The change applies on restart.
  - `IniDoc` is a comment-preserving ini parser. It infers Bool/Choice/Int/Float from the `; N = text` comments and saves atomically.
  - Mods are linked to inis by normalized name, or through `configs/modmenu.ini` `[Links]`.
- **Mods screen (`modmenu.cpp`)**: pressing `mods` opens the Options tab's rows as a mods screen.
  - Each mod shows ON/OFF. Each ini key shows its value, from the ini linked to the mod. Choice keys cycle their labels on arrow press.
  - Mod and ini changes are saved at once. Mod on/off applies after restart (a header row says so).
  - Rows that do not fit a free entry are collapsed. Slider and blank rows are always collapsed.
  - It logs `mods button pressed` and `mods screen: N mods, M entries` on open.
- **Pending**: on-device verification. Only the host core is tested.

## Build / test
```
~/Library/Android/sdk/ndk/27.3.13750724/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-23 APP_ABI=arm64-v8a
c++ -std=c++17 -Wall -o /tmp/test_core test_core.cpp modmenu_core.cpp && /tmp/test_core
```
Deploy `libs/arm64-v8a/libmodmenu.so` to `mods/` and `configs/modmenu.ini` to `configs/`. Then run `adb logcat -s ModMenu`.

Expected log: `installed`, then `names resolved`, then on opening the pause menu `GTASA_PauseMenu_Mobile: inserted 'mods' at 7 (tab class copied from index 5)`.
