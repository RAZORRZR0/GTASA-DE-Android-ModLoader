# Mods menu: design (verified on device, v1.87.0)

## What the user sees
- A **mods** entry in the main menu and the pause menu, placed before *exit*.
- Sub-tabs at the top, the game's own tab bar: **mods**, then one tab per mod (the first 4; more mods stay reachable
  from the list). The game moves the highlight; tapping a mod's row in the list also selects its tab.
- The **mod list** (mods tab): one row per mod, titled from its ini file name (`GTA_Prop_Fix.ini` → "GTA Prop Fix"),
  with On/Off, plus a note that on/off applies after a restart. Disabled mods are listed too.
  - The arrows toggle a mod on/off. This renames `libX.so` ↔ `libX.so.disabled` and takes effect on restart.
  - Tapping the row opens the **mod page**.
- The **mod page**, all on one scrolling page:
  - `Load at Game Start` (the same on/off), and "Settings apply after a restart" when the mod has no live hook;
  - every ini section as a header row, its keys below in file order, labelled from the key name
    (`GameTimecycStrength` → "Game Timecyc Strength"). Each change is saved immediately and atomically, and
    comments are preserved:
    - Bool keys toggle (On/Off);
    - Choice keys (`; N = text` comments) cycle their labels;
    - number keys with a `; range: A..B` comment line are the game's slider row with its number box (Int step 1,
      Float step (B-A)/100), saved when the drag ends; other number keys step by arrows (Int 1, Float 0.1);
    - string keys are shown read-only, because there is no text input.
- **Live changes:** after saving, ModMenu calls `extern "C" void GTAML_OnConfigChanged()` if the loaded mod exports it. Otherwise the page says "Settings apply after a restart". PropFix and SkyGfx implement it.
- **Which ini belongs to a mod:** `configs/modmenu.ini` `[Links] mod=File.ini`, or a normalized-name match.

## How it works
- **Entry:** on `SetTemplate`, insert into the menu template's parallel arrays (`LeftButtons`, `FunctionToCallOnPress`, `SelectorTabs`) a copy of the options tab class. Each template is patched once.
- **Our tab:** the second `UI_Options_Menu_Tab_SA_Mobile_C` built during a patched `SetTemplate`, since tabs are built in index order.
  - We use its first list, `ControlsOptions_C` (a `UI_ScrollingSlotList_C`).
  - Its 5 sub-tab buttons are relabelled via `SetButtonTextValue(Text, false)`; unused ones are collapsed.
  - A tab tap calls `TabUsed(button index @0, selected tab @4)` once per button; after the game ran it (it moves the
    highlight and switches `WidgetSelector`, a `WidgetSwitcher`, to that tab's list) we set the switcher back to
    index 0, since the other lists are the game's real settings, and show the tab's view. Opening a mod from the
    list calls the tab's `SetToTab`.
- **Rows:** `ClearSlotList`, then for each row:
  1. `UWidgetBlueprintLibrary::Create(list, rowClass, null)`;
  2. `AddSlotItem(row, Options, null)`. `Options` is a copy of a live row's options with `PropertyName = "GTAML_Dummy"`.

  `UGameterSettings` Get/Set with an unknown name return defaults and do nothing (`docs/bp_decompiled.md`), so the game's row logic can't touch real settings.
- **Texts:** applied on the next `Tick`, after `InitSlotButton`. Message rows use `OverrideMainText` / `OverrideAltText`; Selection rows use `OptionNameText` / `SelectionText`.
- **Input:** for our rows we swallow the arrow `BndEvt__*OnButtonClickedEvent`, the body (`OnSlotClicked`, other `BndEvt` clicks) and `UpdateSettings` / `FinalCheck` / `Next` / `PreviousSelection`. Then we act ourselves, once per tap.
- **Sliders:** `Options` (`FGTAOptionsItemTemplate`) has `UseSlider` 0x3d, `UseText` 0x3e, `MinSlider` 0x40, `MaxSlider` 0x44, `StepSizeOverride` 0x48 (resolved by name). The range did not reach `MainSlider` from `Options` (it stayed 0..1), so it is set with `SetMinValue`/`SetMaxValue`/`SetStepSize`, then `UpdateSlider`/`UpdateText` show the value. `OnFloatValueChangedEvent` (value @0) is mirrored into the number box and swallowed; `OnMouseCaptureEndEvent` saves and runs the game's own handler, which writes the value under the dummy name (ignored).
- **Rebuilds:** if the game refills the list, its new row Constructs trigger a rebuild on the next `Tick`.
- **Write detector:** while the screen exists, hooks on `UGameterSettings::Set{Bool,Int,Float}Value` log any write to a real setting. None were seen in testing.

## Limits
- String keys are read-only; the slider's number box is display only (no keyboard on mobile).
- Gamepad navigation is untested.

## For mod authors: live settings
Export one function. ModMenu calls it on the game thread, right after your ini was saved:
```cpp
extern "C" __attribute__((visibility("default"))) void GTAML_OnConfigChanged() { LoadConfig(); }
```

## For mod authors: sliders
Put a `range:` line in a number key's comment block; the key then shows as a slider:
```ini
; Strength of the colour filter. 0 = none, 1 = full.
; range: 0..2
Strength=1.0
```
