# Mods menu: design (verified on device, v1.87.0)

## What the user sees
- A **mods** entry in the main menu and the pause menu, placed before *exit*.
- The **mod list**: header "mods (N)", a row "N mods installed · changes apply after restart", and one row per mod. Disabled mods are listed too.
  - The arrows toggle a mod on/off. This renames `libX.so` ↔ `libX.so.disabled` and takes effect on restart.
  - Tapping the row opens the **mod page**.
- The **mod page**, all on one scrolling page:
  - `< back`;
  - an `enabled` row;
  - every key of the mod's ini, in file order:
    - Bool keys and Choice keys (`; N = text` comments) are editable; they are saved immediately and atomically, and comments are preserved;
    - numbers and strings are shown read-only, because there is no range source for sliders yet.
- **Which ini belongs to a mod:** `configs/modmenu.ini` `[Links] mod=File.ini`, or a normalized-name match.

## How it works
- **Entry:** on `SetTemplate`, insert into the menu template's parallel arrays (`LeftButtons`, `FunctionToCallOnPress`, `SelectorTabs`) a copy of the options tab class. Each template is patched once.
- **Our tab:** the second `UI_Options_Menu_Tab_SA_Mobile_C` built during a patched `SetTemplate`, since tabs are built in index order.
  - We use its first list, `ControlsOptions_C` (a `UI_ScrollingSlotList_C`).
  - The other sub-tab buttons are collapsed. The first is relabelled via `SetButtonTextValue(Text, false)`.
- **Rows:** `ClearSlotList`, then for each row:
  1. `UWidgetBlueprintLibrary::Create(list, rowClass, null)`;
  2. `AddSlotItem(row, Options, null)`. `Options` is a copy of a live row's options with `PropertyName = "GTAML_Dummy"`.

  `UGameterSettings` Get/Set with an unknown name return defaults and do nothing (`docs/bp_decompiled.md`), so the game's row logic can't touch real settings.
- **Texts:** applied on the next `Tick`, after `InitSlotButton`. Message rows use `OverrideMainText` / `OverrideAltText`; Selection rows use `OptionNameText` / `SelectionText`.
- **Input:** for our rows we swallow the arrow `BndEvt__*OnButtonClickedEvent`, the body (`OnSlotClicked`, other `BndEvt` clicks) and `UpdateSettings` / `FinalCheck` / `Next` / `PreviousSelection`. Then we act ourselves, once per tap.
- **Rebuilds:** if the game refills the list, its new row Constructs trigger a rebuild on the next `Tick`.
- **Write detector:** while the screen exists, hooks on `UGameterSettings::Set{Bool,Int,Float}Value` log any write. None were seen in testing.

## Limits
- Numeric and string ini keys are read-only. A slider needs min/max, which the ini doesn't provide.
- Gamepad navigation is untested.
