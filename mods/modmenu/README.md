# modmenu

Adds a **mods** entry to the pause menu (and main menu), placed before *exit*. The goal is a native-looking screen where each mod can be turned on/off and its ini edited.

## Status
- **Core (`modmenu_core.*`)**: done and host-tested.
  - `ModList` turns a mod on/off by renaming `x.so` ↔ `x.so.disabled`. The change applies on restart.
  - `IniDoc` is a comment-preserving ini parser. It infers Bool/Choice/Int/Float from the `; N = text` comments and saves atomically.
  - Mods are linked to inis by normalized name, or through `configs/modmenu.ini` `[Links]`.
- **Step A (`modmenu.cpp`)**: entry injection, plus event discovery logs.
  - On `SetTemplate`, it inserts the label `mods`, the function `GTAML_MODS` and a copy of the options tab class into the template's parallel arrays. Each template is patched once.
  - It logs `InteractedButton`, the `CustomFunction*` strings, and the first call of each `UI_PauseMenu_C` function.
- **Pending**: the screen itself. That means rows built from the game's `UI_OptionButton_*_Mobile_C` widgets inside the options tab, plus change handling.

## Build / test
```
~/Library/Android/sdk/ndk/27.3.13750724/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-23 APP_ABI=arm64-v8a
c++ -std=c++17 -Wall -o /tmp/test_core test_core.cpp modmenu_core.cpp && /tmp/test_core
```
Deploy `libs/arm64-v8a/libmodmenu.so` to `mods/` and `configs/modmenu.ini` to `configs/`. Then run `adb logcat -s ModMenu`.

Expected log: `installed`, then `names resolved`, then on opening the pause menu `GTASA_PauseMenu_Mobile: inserted 'mods' at 7 (tab class copied from index 5)`.
