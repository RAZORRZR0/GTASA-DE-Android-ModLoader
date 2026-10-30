# Mods menu: option rows (from `/tmp/gtamod/uidump4.log`)

Notation: **V** = verified by a CLS line (`uidump4.log:LINE`). **G** = guess or inference.

## 0. Read this first: the dump is mostly missing

`uidump4.log` has 1526 lines. Only a few classes are complete; logcat dropped the rest.

- **Complete (props and functions):**
  - `UI_ScrollingSlotList_C` (854-1101)
  - `UI_DesignTimeUserWidget` (1102-1158)
  - `UserWidget` (1159-1309, partial)
  - the head of `UI_PauseMenu_C` (1310-1450)
- **Dropped:** from about line 1444 on, only one line in about ten survives. The affected classes are `UI_OptionsMenuTab_C`, `UI_TabbedLayout_C`, `UI_Options_Menu_Tab_SA_Mobile_C`, `UI_SlotButton_C`, `UI_SlotButton_Selection_C` and `UI_SlotButton_Bool_C`.
  - Only `chain[2] UI_SlotButton_C` (1508) survives for the slot buttons.
  - The `CLS SelectorTabs[i] ->` and `CLS template.*` lines are absent.
  - Only `[0..2]` of the 8 pause-template `LeftButtons` are logged (1462-1470).
- **There are no `CLS inst` lines.** `uidump.cpp:740` filters on the class prefix `UI_OptionButton_`, but the real classes are `UI_SlotButton_*`. See `uidump.log:43-60,156-212`: `UI_SlotButton_{Blank,Game_Mobile,Message_Mobile,Selection_Mobile,Bool_Mobile,Slider_Mobile,Version,CenterMessage(_Mobile),Legend_Mobile}_C`. `UI_OptionButton_*` does not exist in this build, and the filter can never match.
- For questions 1 and 3 the dump cannot answer most of the setter, value and label names. I list what survives. Section 5 gives the logging fix.

### Fixes for the next dump run
1. `uidump.cpp:740`: match `UI_SlotButton_` (any suffix) and `UI_Options_Menu_Tab*`. Call `DumpClass` and `DumpInstanceOnce` on first `Construct`.
2. Logcat chatty-drops bursts. Throttle `Out()` in `DumpClass`/`DumpParams` (`usleep(2000)`), or write to a file in the app's external dir.
3. Skip `[-]` locals in `DumpParams` (about 5x fewer lines).

## 1. Class hierarchy, functions and properties

### 1.1 Hierarchy

| Class | Superclass chain | Evidence |
|---|---|---|
| `UI_ScrollingSlotList_C` | `UI_DesignTimeUserWidget` -> `UserWidget` -> ... | V 855, 1102, 1159 |
| `UI_SlotButton_Bool_C`, `UI_SlotButton_Selection_C` | superclass is `UI_SlotButton_C` | V 1508 follows the Selection lines 1505-1507 and precedes the Bool lines 1509-1511 |
| `UI_SlotButton_C` | parent unknown | no line survives |
| `UI_SlotButton_{Bool,Selection,Slider,Message}_Mobile_C` | **G**: subclass of the `_C` of the same kind. `Blank` has only `UI_SlotButton_Blank_C`. | `uidump.log:43,53-60` |
| `UI_Options_Menu_Tab_SA_Mobile_C`, `UI_Options_Menu_Tab_Game_SA_Mobile_C` | **unknown.** `UI_OptionsMenuTab_C` / `UI_OptionsMenuTab_Game_C` (V 1471-1473) and `UI_TabbedLayout_C` (V 1474-1477) exist. Linking them is **G**. | |
| `UI_PauseMenu_C` | -> `UI_Menu_Base_BP_C` -> `UI_Menu_Base` | V: 1445-1459 members |

The rows are `UI_SlotButton_*_Mobile_C`, not `UI_OptionButton_*_Mobile_C`.

### 1.2 `UI_ScrollingSlotList_C` (complete, V). Properties

| Offset | Property | Type |
|---|---|---|
| 0x290 | UberGraphFrame | struct |
| 0x298 | MainScrollBox | ScrollBox* |
| 0x2a0 | ScrollingCanvas | CanvasPanel* |
| 0x2a8 | EmptyButtonType | TSubclassOf |
| 0x2b0 | HasConstructed | bool |
| 0x2b8 | ParentTabbedLayout | soft `UI_TabbedLayout_C` |
| 0x2e0 | SlotStyle | struct `ScrollListStyle` |
| 0x438 | RunInitOnConstruct | bool |
| 0x440 | LastUserSelectedWidget | soft UserWidget |
| 0x468 | SlotClasses | struct `SlotListSlotClasses` (40 B) |
| 0x490 | InteractSlotButtonSound | soft SoundWave |
| 0x4b8 | OverrideSlotFocus | int32 |
| 0x4bc | DidClearMouseFocus | bool |
| 0x4c0 | OnSelectedTab | multicast delegate |
| 0x4d0 | OnDeselectedTab | multicast delegate |
| 0x4e0 / 0x4e1 | bUseHapticNavigation / bUseHapticInteract | bool |
| 0x4e4 | ScrollbarThickness | float |
| 0x4e8 | ScrollBarVisibility | enum |
| 0x4f0 | VersionString | FText |
| 0x508 | VersionClass | TSubclassOf |

Lines 856-876. The parent `UI_DesignTimeUserWidget` adds `HasTextToBeLocalized` 0x260, `WidgetsRequiringLoc` 0x268, `OriginalLocKeys` 0x278 and `bTriedSocialClubResolve` 0x28c (1103-1106). `UserWidget.WidgetTree` is 0x1d8 (1170).

**Functions with real params (V)**

| Function | Params | Line |
|---|---|---|
| `AddSlotItem` | `Slot Item`: `UI_SlotButton_C*` [Parm]; `Options`: struct `GTAOptionsItemTemplate` [Parm]; `SettingsObject`: `GameterSettings*` [Parm] | 1039-1042 |
| `ClearSlotList` | none | 971 |
| `CreateScrollingListItems` | `DEBUG_ALL_ITEMS`: bool [Parm] | 1052-1053 |
| `ScrollingList Init`, `SetMenu`, `SetFocusToFirstEntry`, `SetupNavigation`, `UpdateFocus`, `DelayedSetFocusToFirst`, `SetVersionText`, `UpdateControllerFocusVisability` | none | |
| `Grab Slot Focus` | `ForceIgnoreSound`: bool [Parm]; `SelectedSlotButton`: `UI_SlotButton_C*` [Parm\|Out] | 1011-1013 |
| `FocusSpecificSlot` | `SlotIndex`: int32 [Parm]; `WasValid`: bool [Parm\|Out]; `SelectedSlotButton` [Parm\|Out] | 942-945 |
| `GrabSpecificOptionFocus` | `PropertyName`: Name [Parm]; `FocusedWidget`: Widget* [Parm\|Out] | 953-955 |
| `OnOptionPressed` | `PressedButton`: `UI_SlotButton_C*` [Parm] | 877-878 |
| `SlotButtonPressed` (custom event) | `InteractionType`: byte [Parm]; `BoundValue`: int32 [Parm]; `ButtonSentFrom`: `UI_SlotButton_C*` [Parm] | 1081-1084 |
| `UpdateSelectedSlot` | `SelectedItem`: `UI_SlotButton_C*` [Parm]; `FromHover`: bool [Parm] | 1024-1026 |
| `IsCurrentInputController` | ret bool | 935-936 |
| `OnMouseWheel` / `OnMouseButtonDown` / `OnMouseMove` | `MyGeometry` [Parm]; `MouseEvent` [Parm\|Out]; ret `EventReply` | 884-906 |
| `OnSelectedTab_Event`, `OnSelectedTab__DelegateSignature`, `OnDeselectedTab__DelegateSignature`, `Construct`, `OnSynchronizeProperties`, `Tick(MyGeometry, InDeltaTime)` | | |

**Inherited from `UI_DesignTimeUserWidget` (V, 1102-1158)**

- `GetGameterface()` -> `Gameterface*`
- `GetNextValidSelectionIndex(_OptionsItem: GTAOptionsItemTemplate [Parm|Out], _SelectionNames: TArray<FText> [Parm|Out], _Forward: bool, _CurrentIndex: int32)` -> int32 (1139-1144)
- `UpdateLanguage` -> bool
- `GetLocalizedStringFromKey(Key: FString)` -> FString
- `PlayMenuSoundNative(saoundWave, Volume: float)`
- `HandleBack`, `TakeFocus` -> bool
- `GetWorldSettings` -> `WorldSettings*`

### 1.3 `UI_SlotButton_C` and the row classes: only fragments survive

| Class | Item | Evidence |
|---|---|---|
| `UI_SlotButton_Selection_C` | property `NavButtonSize` (float) @ **0x718** | V 1505 |
| `UI_SlotButton_C` | `OnAnalogValueChanged(InAnalogInputEvent: struct [Parm])` | V 1489 |
| `UI_SlotButton_C` | `FixDisplayName(IsSettingAvailable: bool [Parm])` | V 1492 |
| `UI_SlotButton_C` | `SetHeight(NewHeight: float [Parm])` | V 1499 |
| `UI_SlotButton_C` | `OnOptionSelectionChanged__DelegateSignature(FromHover: bool [Parm])` | V 1503 |
| `UI_SlotButton_C` | `OnOptionChange__DelegateSignature(OptionName: Name [Parm])` | V 1504 |

**Functions known by name only** (real params dropped, locals visible):

- `UI_SlotButton_C`: `OnSelected`, `DidConfirmPostConfirmation`, `On Option Changed Final Check` (reads `GameterSettings`), `ShouldShowConfirmation` (casts to `UI_Menu_Base_BP_C`), `IsInputTypeController`.
- `UI_SlotButton_Bool_C`: `NavigateLeft`, `InitSlotButton`, `UpdateSettingForConfirmation` (reads `GameterSettings`).
- `UI_SlotButton_Selection_C`: `GetEntriesForEmptySelection` (reads `GameterSettings`).

**Not in the dump:**

- `UI_SlotButton_Slider_Mobile_C`, `_Message_Mobile_C`, `_Blank_C` and every `*_Mobile_C`.
- The label, value, min/max and description properties of every row.
- `UI_SlotButton_Slider_Mobile_C::GetPercent` appears only as a ProcessEvent trace (`uidump.log:212`).

### 1.4 Tab classes (fragments only)

| Class | What survives | Evidence |
|---|---|---|
| `UI_Options_Menu_Tab_SA_Mobile_C` | `Tick(MyGeometry)`; `ExecuteUbergraph_UI_Options_Menu_Tab_SA_Mobile` | V 1481-1482 |
| `UI_Options_Menu_Tab_Game_SA_Mobile_C` | nothing in uidump4 | |
| `UI_OptionsMenuTab_C` | `AddContentToSelector(ContentCreated: UserWidget* [Parm\|Out])`; `SetToTab` | V 1472-1473 |
| `UI_OptionsMenuTab_Game_C` | `RefreshSaveTabState` | V 1471 |
| `UI_TabbedLayout_C` | `HoverSound` @ **0x568** (soft SoundWave); `SetupTabs(TabsButtonsCreated: TArray<UI_Button_Base_C*> [Parm\|Out])`; `UpdateButtonsOnInteract` | V 1474-1477 |

The tab's own property list (the one holding the `UI_ScrollingSlotList_C`) was dropped. Also (V): `UI_Menu_Base_BP_C.UpdateButtonsOnInteract(InteractButton: byte)`, `GetLanguageTab(IndexInWidgetSelector: int32 [Parm|Out])`; `UI_Menu_Base.CustomFunctionFromString(FunctionString: FString)`, `CustomFunctionFromStringWithText(UsableText: FText [Parm|Out])`.

## 2. How the tab builds rows

- **V:** the slot list adds a row with `UI_ScrollingSlotList_C::AddSlotItem(Slot Item: UI_SlotButton_C*, Options: GTAOptionsItemTemplate, SettingsObject: GameterSettings*)` (1039-1042).
- **V (from the locals, 1043-1051), the body does this:**
  1. `GetChildrenCount`, `Percent_IntInt` and a `SlateColor` select pick an alternating row colour.
  2. It converts the `InteractSlotButtonSound` and `ScrollingSlotList` soft references, so it assigns the list and sound to the row.
  3. `AddChild` returns a `PanelSlot`, so the row goes into `MainScrollBox` or `ScrollingCanvas` (**G**).
- **V:** rows are created with `CreateWidget`. `ScrollingList Init` creates a `UI_SlotButton_Blank_C` (1074). `SetVersionText` creates a `UI_SlotButton_C` and casts it to `UI_SlotButton_Version_C` (881-882).
- **Which caller fills a tab:** unknown. It is probably the tab's `ExecuteUbergraph_UI_Options_Menu_Tab_SA_Mobile` (**G**).
- **Row classes:** the template `GTAMainMenuUITemplate` has `SliderSlotClass` 0x980, `SelectionSlotClass` 0x988, `BoolSlotClass` 0x990, `MessageSlotClass` 0x998 and `SaveGameSlotClass` 0x9A0 (`reflection_layout.md` §4). `uidump.log:53-60` shows that `*_Mobile_C` classes are what gets instantiated.
- **Data source:**
  - `GTAOptionsItemTemplate` is the per-row struct, passed by value. Its layout was not dumped. `GetNextValidSelectionIndex` takes it together with a `TArray<FText>` of selection names, so choices are separate FText names (V).
  - `UGTAOptionsList` has `Category` (byte) at 0x30 and `Options` (`TArray<struct>`) at 0x38 (`reflection_layout.md`). **G:** the tab reads this list. Its property on the tab was dropped.
  - `GameterSettings*` is the second data argument. Rows read and write through it (V: three locals named `GetSettings`).
  - `GrabSpecificOptionFocus(PropertyName: Name)` (V 954) walks the children and compares a Name. So each row stores a property Name.
  - `ScrollingList Init` calls `ShouldShowFullOptions` on a Gameterface (V 1068). **G:** the Gameterface filters which options appear.

## 3. Label, value, description per row class

**Not answerable from the dump.** The properties and functions needed are in dropped lines.

- **V:** `UI_SlotButton_C::FixDisplayName(IsSettingAvailable: bool)` builds a display name via `Conv_StringToText` and `MakeStruct FormatArgumentData`. The label source (the struct or a button property) is **G**.
- **V:** `UI_SlotButton_Slider_Mobile_C::GetPercent` is called on Tick. The slider value is a 0-1 percent. The member names (value, min, max) are unknown.
- **V:** `UI_SlotButton_Selection_C::GetEntriesForEmptySelection` reads `GameterSettings`. Choices are `TArray<FText>`.
- **V:** `UI_SlotButton_Bool_C::InitSlotButton` exists. It is probably where label and value are set (**G**), and its params are unknown.

**How a change is reported.** All candidates are verified to exist, and none is verified as the catch point:

- `UI_SlotButton_C::OnOptionChange__DelegateSignature(OptionName: Name)` is a multicast delegate. A broadcast does not run through `ProcessEvent` on the signature function. `ProcessEvent` sees the bound BP handler on the tab or list instead. Its name is unknown.
- `UI_ScrollingSlotList_C::SlotButtonPressed(byte InteractionType, int32 BoundValue, UI_SlotButton_C* ButtonSentFrom)` is likely a funnel for row interactions, with `BoundValue` carrying the value or index. **G.**
- `UI_ScrollingSlotList_C::OnOptionPressed(UI_SlotButton_C* PressedButton)`.
- `UI_SlotButton_C::On Option Changed Final Check` (no visible params).

So the hook should log `ProcessEvent` on `UI_ScrollingSlotList_C` and `UI_SlotButton_*` objects. Match `ButtonSentFrom` or `PressedButton` against our own rows.

## 4. Recipe (V = function exists with these params, G = guess)

`tab` = the `UI_Options_Menu_Tab_SA_Mobile_C` in `WidgetSwitcher 'SelectorWidget'`. `list` = its `UI_ScrollingSlotList_C`.

1. **Find `list`.** G. Walk `tab.WidgetTree` (0x1d8, V 1170) to the first `UI_ScrollingSlotList_C`, or scan the object array.
2. **Clear:** `ProcessEvent(list, FindFunctionByName("ClearSlotList"), nullptr)`. V (971): no params. What it clears besides children is G.
3. **Create the Bool row widget.** G. Read `BoolSlotClass` from the template (`tmpl+0x990`). Create it with `UUserWidget::CreateWidgetInstance` (see `reflection_layout.md` §5 for the exports). The class that actually runs is `UI_SlotButton_Bool_Mobile_C` (`uidump.log:57-58`).
4. **Build `GTAOptionsItemTemplate`.** G (layout unknown). Plan: copy an existing Bool entry from the options list and patch its name fields.
5. **Add:** `ProcessEvent(list, FindFunctionByName("AddSlotItem"), parms)`. Param order and types are V: `{UI_SlotButton_C* "Slot Item"; GTAOptionsItemTemplate Options; GameterSettings* SettingsObject}`. Read the offsets at runtime from the `UFunction` param properties.
   - `SettingsObject` is G: use the Gameterface settings.
   - Risk (unverified): `UpdateSettingForConfirmation` and `On Option Changed Final Check` read `GameterSettings`. A made-up `PropertyName` may crash or log an error.
6. **Set label and value.** G, with unknown functions. Candidates: `InitSlotButton`, `FixDisplayName(bool)`, or properties on the row.
7. **Detect a toggle.** G. In the hook, watch `SlotButtonPressed`/`OnOptionPressed` on `list` whose button argument is our row, or the bound `OnOptionChange` handler.
8. **Finish.** Call `SetupNavigation` and `SetFocusToFirstEntry` (V, no params) so focus works (G that it is needed).

## 5. Remaining unknowns and the smallest log that resolves each

| # | Unknown | Smallest log |
|---|---|---|
| 1 | Layout of `GTAOptionsItemTemplate` | In `Observe`, on `AddSlotItem` for a `UI_ScrollingSlotList_C`, log `func->ParmsSize`, the `Options` param's `FStructProperty::Struct` (@0x78), and walk its `ChildProperties` with the existing `DumpProps`. Hex-dump `parms` and the strings in `FText`/`FName` fields, for the first Bool, Slider, Selection and Message calls. |
| 2 | Property names for label, value, min/max, description, `PropertyName` on each row class | Fix the `AfterCall` filter. Run `DumpClass` and `DumpInstanceOnce` on the first `Construct` of each `UI_SlotButton_*_Mobile_C`, throttled. |
| 3 | Setter functions and params per row class | The same class dump (`CLS <Class> fn ...`). |
| 4 | Which function fires on a toggle | While toggling a Bool row in the real Options tab, log every `ProcessEvent` whose object is a `UI_SlotButton_*`, `UI_ScrollingSlotList_C` or `UI_Options_Menu_Tab_*`. Skip `Tick`, `OnMouseMove`, `GetPercent`. |
| 5 | Where `Options` and `SettingsObject` come from | Log parms of `CreateScrollingListItems` and the tab's `ExecuteUbergraph_...`, plus `DumpClass` of the tab (to find the `GTAOptionsList` property). |
| 6 | Tab superclass chain | `chain[n]` from `DumpClass(tab_cls)`. |
| 7 | Side effects of `ClearSlotList` | Log `list+0x440` and `MainScrollBox` child count before and after. |
| 8 | Whether a made-up `PropertyName` survives `GameterSettings` | Add a row, toggle it, and log `UI_SlotButton_*` calls and `E/UE4` lines. |
