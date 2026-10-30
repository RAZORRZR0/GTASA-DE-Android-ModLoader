# Mods menu: design and findings

## Entry injection (verified on device, v1.87.0)
- `ProcessEvent` "SetTemplate" on `UI_MainMenu_C` / `UI_PauseMenu_C` passes the template asset in the first 8 bytes of parms. The assets are `GTASA_MainMenu_Mobile` and `GTASA_PauseMenu_Mobile`.
- modmenu inserts the following before *exit* in the parallel arrays (`LeftButtons`, `FunctionToCallOnPress`, `SelectorTabs`). Each template is patched once.

  | Array | Inserted value |
  |---|---|
  | LeftButtons | label "mods" |
  | FunctionToCallOnPress | "GTAML_MODS" |
  | SelectorTabs | the options tab class |

  The button renders in both menus: main menu index 4, pause menu index 7. Tapping it opens a copy of the Options screen.
- `GTAML_MODS` never reaches `CustomFunctionFromString`. A non-empty `SelectorTabs` entry opens the tab instead.

## Click events
`InteractedButton(int32 type, int32 index, ...)` fires on the menu (`UI_MainMenu_C` / `UI_PauseMenu_C`).

| type | Meaning |
|---|---|
| 1 | press |
| 2 | release |
| 0 | click |
| 3 | hover |
| 4 | unhover |

**Trigger for the Mods screen:** type 0 with index equal to the inserted index.

## Next: the screen
- After the trigger, find the tab widget that was just shown in `WidgetSwitcher 'SelectorWidget'`. It is a `UI_Options_Menu_Tab_SA_Mobile_C` containing a `UI_ScrollingSlotList_C`.
- Clear it (`UI_ScrollingSlotList_C::ClearSlotList`), then add rows:
  - Bool rows for mods;
  - Bool, Selection or Slider rows for ini keys, using the template's `BoolSlotClass` / `SelectionSlotClass` / `SliderSlotClass`.
- Still unknown: the row setter and change-event functions. Get them from the `CLS` dumps in `uidump4.log`.
- Backend: `mods/modmenu/modmenu_core.*`, host-tested.
