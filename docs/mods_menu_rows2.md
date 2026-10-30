# Mods menu rows, round 2 (from /tmp/gtamod/uidump5.txt, 16695 lines)

Tags: **V** = shown by a capture line (`uidump5:LINE`). **I** = inference from V lines. **G** = guess.

## 0. What the capture does NOT contain

- **No `GTAOptionsItemTemplate` field dump.** The file has no `CLS` line for the struct itself. The only mentions are:
  - `UI_SlotButton_C.Options : StructProperty struct=GTAOptionsItemTemplate off=0x298 elem=144` (719)
  - the `AddSlotItem` `Options` param (9612)
  - the `GetNextValidSelectionIndex` `_OptionsItem` param (264)
- **No `AddSlotItem` call and no struct hex dump.** `grep AddSlotItem` returns only the five `CLS ... fn/param` lines, 9610-9613.
- **No `CLS inst ...Options` values.** The `CLS inst` dumper skips struct properties.
- **Why the trace is blind to this.** The EV hook sees only calls that go through `UObject::ProcessEvent`. Blueprint-to-Blueprint calls go through the BP VM and bypass it. Neither `AddSlotItem`, `InitSlotButton`, `UpdateSettings`, `NextSelection` nor `UpdateSettingForConfirmation` ever appears as an EV line, although they must have run while the tabs were populated. **I.**
  - What EV does show is events that native code or delegates dispatch: `Construct`, `BndEvt__*`, `OnMotionDetected`, delegate handlers such as `ControlsOptions_C::OptionChanged`, and `On Option Changed Final Check`.
- **No selection-value change in the EV log.** There is no `UI_SlotButton_Selection_Mobile_C` event other than Construct and Destruct. The only `OnOptionSelectionChanged` lines are focus changes (see section 4).
- **`ControlsOptions_C`, `GameplayOptions_C`, `GraphicsOptions_C`, `AudioOptions_C`, `LanguageOptions_C` and `UI_OptionsList_Game_C` have no CLS dump.** Their superclass and properties are unknown.

So questions 1 and 2 are only partly answerable from this file. Section 6 gives the one-shot runtime dump that closes the gaps.

## 1. `GTAOptionsItemTemplate`

| Fact | Evidence |
|---|---|
| Size is 144 bytes (0x90) | `UI_SlotButton_C.Options` at 0x298, elem=144 (719) |
| It lives at `row+0x298..0x328` and is followed by `FunctionBindNum` at 0x328 | 719-720 |
| It is passed by value as `Options` in `AddSlotItem` | 9612 |
| It is passed by value as `_OptionsItem` (with `[Parm\|OutParm]`) in `GetNextValidSelectionIndex`, together with a `TArray<FText> _SelectionNames` | 264-265 |
| Field names, types and offsets | **not captured** |
| Values for a Bool, Slider or Selection row | **not captured** |

Where the row reads its own copy: `UI_SlotButton_C.Options` is an embedded copy of the struct, so the row owns its config after `AddSlotItem`. **I.**

Runtime recovery (**G**, but straightforward):
- Read the live Bool, Slider and Selection rows (`UI_SlotButton_C`, class dumped at 718-755) at `row+0x298`, 144 bytes each.
- Walk the struct's `ChildProperties` via the `FStructProperty::Struct` pointer of the `Options` property (the same pointer the CLS dumper already has).
- The `FName`s and `FText`s in the hex will show which field is `PropertyName`, the label, and so on.

## 2. Who calls `AddSlotItem`, and in what order

**Not observed.** What the capture does show, as widget `Construct` order. Rows are constructed once added to a live tree, so this is the order in which `AddSlotItem` ran. **I.**

**Container structure.** Pause menu `LeftButtons` and `SelectorTabs` (10337-10366):
- Tab 2 `game` -> `UI_Options_Menu_Tab_Game_SA_Mobile_C`. It holds `UI_OptionsList_Game_C` with `UI_SlotButton_Game_Mobile_C` rows (698-955, 10628-10666).
- Tab 5 `options` -> `UI_Options_Menu_Tab_SA_Mobile_C` (10363).
- **Tab 7 `mods` (`FunctionToCallOnPress=GTAML_MODS`, 10354-10355) -> the same class `UI_Options_Menu_Tab_SA_Mobile_C` (10365).** So the mods tab is a second, independent instance of the Options tab. This is why `UI_Options_Menu_Tab_SA_Mobile_C::Construct` appears twice, at 11246 and 11402 (an earlier pair is at 1080 and 1828).
- Slot classes from the template (10551-10554):

| Template property | Class |
|---|---|
| `SliderSlotClass` | `UI_SlotButton_Slider_Mobile_C` |
| `SelectionSlotClass` | `UI_SlotButton_Selection_Mobile_C` |
| `BoolSlotClass` | `UI_SlotButton_Bool_Mobile_C` |
| `MessageSlotClass` | `UI_SlotButton_Message_Mobile_C` |

**Class hierarchy of the Options tab** (60-110, 1081-1102): `UI_Options_Menu_Tab_SA_Mobile_C` -> `UI_Options_Menu_Tab_SA_C` -> `UI_OptionsMenuTab_C` -> `UI_TabbedLayout_C` -> `UI_DesignTimeUserWidget`.
- `UI_OptionsMenuTab_C` has `AddContentToSelector(Widget Class: Class, Selector to add to: WidgetSwitcher, ContentCreated: UserWidget [out])` (122-125).
- The tab is therefore a `TabbedLayout` whose `WidgetSelector` (a `WidgetSwitcher`, `UI_TabbedLayout_C` 0x2c8) holds one list per sub-tab.

**What happens on tab `Construct`** (first Options tab, 1080-1977; the second instance repeats it at 1828-1973 and 11246-11514 and 11402-11514):
1. `UI_Options_Menu_Tab_SA_Mobile_C::OnInitialized`, `PreConstruct`, `Construct` (1077-1080).
2. Five list widgets are then constructed in order. Each is followed by the `Construct` events of its row widgets:

| List widget (line of `Construct`) | Row classes in Construct order |
|---|---|
| `ControlsOptions_C` (1109) | Message, Selection, Bool, Bool, Slider, Bool, Slider, Slider, Message (1112-1706) |
| `GameplayOptions_C` (1710) | Selection, Bool, Selection, Message, `Blank_C`, `Blank_C` (1713-1730) |
| `GraphicsOptions_C` (1734) | Slider, Bool x4, Slider, Slider, Message (1737-1758) |
| `AudioOptions_C` (1762) | Slider x3, Bool, Selection, Message (1765-1780) |
| `LanguageOptions_C` (1784) | Selection, Bool, Slider, Bool, Slider, Selection, Message x6 (1787-1820) |

3. Then `UI_SC_Achivements_C` (1824), and `UI_SlotButton_Version_C` (1977).

- **Which function runs `AddSlotItem` is unknown.** Most likely it is the BP `Construct` or `CreateScrollingListItems` of each list widget. **G.**
- **Parent of those list widgets:** `ControlsOptions_C` has functions named `OnOptionSelectionChanged`, `OptionChanged`, `RefreshAdjustableControls`, `OnMotionDetected` (V in EV lines). `UI_ScrollingSlotList_C` (9551-9640, one `Construct` at 9640) is the only class in the dump that owns `AddSlotItem`, `ClearSlotList`, `GrabSpecificOptionFocus` and so on. So these are probably subclasses of `UI_ScrollingSlotList_C`. **I.**
- **`SettingsObject` class:** `GameterSettings` (`AddSlotItem` param `SettingsObject : ObjectProperty class=GameterSettings`, 9613). The same param is on every row's `InitSlotButton` (843, 982, 1149, 1294, 1443, 1616). No instance, address or property list of it appears in the capture. So the property names it exposes are unknown, other than the two seen in EV: `InvertLook` and `TouchSensitivity`.
- **`Gameterface` instance:** `BP_SanAndreasInterface_C @0x70dd18c020`. It is printed after each `UI_MainMenu_C::CustomFunction*` call (e.g. 2119).
  - `UI_MainMenu_C::CustomFunctionFromString("RefreshOptionsKeepSelection")` is called once at 8818, when the Game tab is torn down.

## 3. Row classes: label, value, range, choices, description

**Base `UI_SlotButton_C`** (718-875). Superclass chain: `UI_SlotButton_C` -> `UI_DesignTimeUserWidget` -> `UserWidget`.

| Offset | Property | Meaning |
|---|---|---|
| 0x298 | `Options` (144 B struct) | row config |
| 0x328 | `FunctionBindNum` (int) | |
| 0x330 | `SlotDispatcher` (multicast) | signature `(InteractionType: byte, BoundValue: int, ButtonSentFrom: UI_SlotButton_C*)` (875-878) |
| 0x340 | `IsSelected` | focus highlight |
| 0x348 | `ParentScrollingSLot` | soft `UI_ScrollingSlotList_C` |
| 0x370..0x3e8 | `TextColor`, `SelectedTextColor`, `NormalBGColor`, `SelectedBGColor` | `SlateColor` |
| 0x410 | `DefaultTextFont` | `SlateFontInfo` |
| 0x468 | `InteractedSound` | |
| 0x490 | `bLockBehavior` | |
| 0x498 | `DidUpdateSettings` | multicast `(RequiresOptionRefresh: bool, PropertyNameToSelect: Name)` |
| 0x4b0 | `ConfirmationRequired` | |
| 0x4c0 | `Has Changed since last save` | |
| 0x4c8 | `PostConfirmationRequired` | |
| 0x4d8 | `bDisabled` | |
| 0x570 | `OnOptionChange` | multicast `(OptionName: Name)` |
| 0x580 | `OnOptionSelectionChanged` | multicast `(OptionName: Name, Selected: bool, FromHover: bool)` |
| 0x592 | `UsesLeftRightNavigation` | |
| 0x5c8 | `OnFinalConfirmation` | multicast `(RequiresOptionRefresh: bool, PropertyNameToSelect: Name)` |

Base functions (all V):

| Function | Params | Line |
|---|---|---|
| `OverrideMainText` | `NewText: FText` | 840-841 |
| `OverrideAltText` | `NewText: FText` | 838-839 |
| `InitSlotButton` | `SettingsObject: GameterSettings*` | 842-843 |
| `UpdateSettings` | `OverrideConfirmationLockout: bool` | 836-837 |
| `GetCurrentSettingText` | `NewParam: FText` (out) | 798-799 |
| `GetSelectionOptions` | `PropertyName: Name`, `DefaultOptions: TArray<FText>` (out), ret `TArray<FText>` | 757-760 |
| `FixDisplayName` | `IsSettingAvailable: bool`, `OutText: FText` (out) | 777-779 |
| `SetDisabled` | `Disable: bool`, `ErrorCode: int` | 788-790 |
| `SetSelected` | `Selected: bool`, `FromHover: bool` | 826-828 |
| `ShouldShowConfirmation`, `DidConfirm`, `DidConfirmPostConfirmation` | confirmation flow | 792-804 |
| `On Option Changed Final Check` | `RequiresOptionRefresh: bool` @0, `PropertyNameToSelect: Name` @4 | 795-796 |
| `UpdateSettingForConfirmation` | | 797 |
| `SetHeight`, `SetFont`, `SetBackgroundColor`, `UpdateBGColor`, `UpdateTextColor` | display refresh | |

**Bool `UI_SlotButton_Bool_C`** (`_Mobile_C` subclass at `UberGraphFrame` 0x708; 1392-1500).
- Label: `OptionNameText` (TextBlock 0x630). Value text: `SelectionText` (TextBlock 0x638).
- `SelectionNames` (`TArray<FText>` 0x648) holds the ON/OFF captions. `SelectedElement` (int 0x658) is the index, 0 or 1.
- `IgnoreSettings` (bool 0x65c). Semantics unverified (**G**: the row neither reads nor writes `GameterSettings`).
- Widgets: `BackButton` 0x5e0, `ForwardButton` 0x600, `SlotButton` 0x640, `Image` 0x620, `MainSizeBox` 0x628. `NavButtonSize` (float) is at 0x704.
- Functions: `NextSelection`, `PreviousSelection`, `ToNumSelection(SelectionValue: int)`, `UpdateSettings(bool)`, `GetCurrentSettingText`, `InitSlotButton`, `InteractedWithSelectionSlot(InteractionType: byte, BoundValue: int, SlotButton: UI_SlotButton_C*)` (1442-1464).
- Description: no property found. Min/max/step: n/a.

**Selection `UI_SlotButton_Selection_C`** (`_Mobile_C` at `UberGraphFrame` 0x720; 1223-1343).
- Same widgets, `SelectionNames` 0x648, `SelectedElement` 0x658 (1249, 1340). `BoolSelection` (`TArray<FText>`) is at 0x660.
- `IgnoreSettings` is at **0x70c** (differs from Bool). `NavButtonSize` 0x718.
- Choices come from `SelectionNames`. `UI_SlotButton_Selection_C.GetEntriesForEmptySelection(EntryName: Name)` fills them from the settings (1269-1272 region).
- Functions: `NextSelection`, `Previous Selection` (with a space), `ToNumSelection`, `UpdateSettings`, `GetCurrentSettingText`, `InitSlotButton` (1273-1312).

**Slider `UI_SlotButton_Slider_C`** (`_Mobile_C` at `UberGraphFrame` 0xb38; 1556-1653).
- Label: `OptionNameText` 0x600. Widgets:

| Offset | Widget |
|---|---|
| 0x5e0 | `EditableTextBox` (typed value) |
| 0x5f8 | `MainSlider` (UMG `Slider`) |
| 0x608 | `SliderBar` (`ProgressBar`) |
| 0x610 | `SliderScaleBox` |
| 0x618 | `SliderSizeBox` |
| 0x620 | `SlotButton` |

- `bIsMouseCaptured` 0xb30.
- Value is a float on `MainSlider`. Observed `Value` goes 51.8 to 72.7 (EV 13021-13099, 14455-14481), so the range is about 0-100 (**I**).
- **Min/max/step are not captured:** no `Slider` class dump and no `CLS inst` of `MainSlider`. Read them at runtime from the UMG `Slider` (`MinValue`, `MaxValue`, `StepSize`).
- Functions: `UpdateSlider(InValue: float)`, `UpdateText(InValue: float)`, `UpdateSliderWithValue(Slider: Slider*, NewValue: float)`, `GetPercent` -> float, `GetSliderStepSizeForController` -> float, `UpdateSettings(bool)`, `InitSlotButton`, `InitSliderStyle`, `GetCurrentSettingText`, `InteractedWithSliderSlot` (1586-1625).
- Commit is on text entry too: `BndEvt__TextBox_..._OnEditableTextBoxCommittedEvent(Text: FText, CommitMethod: byte)` (1619-1621). Not exercised in the capture.

**Message `UI_SlotButton_Message_C`** (`_Mobile_C` at 0x618; 1113-1177).
- `OptionNameText` (left) 0x5f8, `RightText` 0x608, `LeftScale` 0x5e0, `RightScale` 0x600, `MainButton` 0x5e8, `MainSizeBox` 0x5f0.
- `JustLeftMessage` (bool 0x610), `DidConstruct` 0x611.
- Functions: `OverrideMainText(NewText)`, `OverrideAltText(NewText)`, `SetJustLeftMessage(bool)` (1136-1158).
- No value, min/max, choices or description.

**Blank `UI_SlotButton_Blank_C`** (706-709): `Image` 0x5d8, `MainSizeBox` 0x5e0, `SlotButton` 0x5e8. Functions: `SetHeight`, `GetSlotButton`, `SetBackgroundColor`. No text, no value, no `Mobile` subclass.

**Description text:** no row class has a description property. It is not in the capture. **G:** it, if it exists, lives in the `Options` struct.

**Functions that refresh the display:** `UpdateText`, `UpdateSlider`, `UpdateSliderWithValue`, `GetPercent` (Slider); `UpdateTextColor`; `UpdateBGColor`; `SetBackgroundColor`; `FixDisplayName`; `GetCurrentSettingText`; `UpdateSettings`. All of these are BP-internal. The captured evidence of a display refresh is the second `BndEvt...OnFloatValueChangedEvent` echo after a commit (13098).

## 4. EV sequences per user change

Parameter offsets below are V from the `off=` field of each EV line.

**Bool toggle (InvertLook). Click on the forward arrow (11955-11965):**
```
11955 Bool_Mobile_C::BndEvt__ForwardButton_..._5_OnButtonReleasedEvent
11956 Bool_Mobile_C::BndEvt__ForwardButton_..._1_OnButtonClickedEvent         (no params)
11957 ControlsOptions_C::OnOptionSelectionChanged  OptionName@0=InvertLook  Selected@8=true  FromHover@9=false
11961 Bool_Mobile_C::On Option Changed Final Check  RequiresOptionRefresh@0=false  PropertyNameToSelect@4=InvertLook
11964 ControlsOptions_C::OptionChanged              OptionName@0=InvertLook
11966 Bool_Mobile_C::BndEvt__ForwardButton_..._9_OnButtonHoverEvent
```
- Before the click: `OnPreviewMouseButtonDown` and `BndEvt..._3_OnButtonPressedEvent` (11893-11897).

**Bool, second toggle, back arrow (15507-15540):**
```
15507 BndEvt__BackButton_..._2_OnButtonPressedEvent
15525 BndEvt__BackButton_..._4_OnButtonReleasedEvent
15526 BndEvt__BackButton_..._0_OnButtonClickedEvent
15527 ControlsOptions_C::OnOptionSelectionChanged  InvertLook  Selected=true
15531 ControlsOptions_C::OnOptionSelectionChanged  TouchSensitivity  Selected=false
15535 Bool_Mobile_C::On Option Changed Final Check  InvertLook
15538 ControlsOptions_C::OptionChanged  InvertLook
```
- There is no value param in any event: **the new bool is not carried**. Read `row+0x658 SelectedElement` (0/1) after `Final Check`, or call `GetCurrentSettingText`.

**Slider move (TouchSensitivity, 13015-13104):**
```
13015 Slider_Mobile_C::OnTouchMoved
13019 BndEvt__MainSlider_..._3_OnMouseCaptureBeginEvent
13020 BndEvt__MainSlider_..._0_OnFloatValueChangedEvent  Value@0 = 58.23 (13021)
13022-13029 OnOptionSelectionChanged InvertLook false / TouchSensitivity true     (focus moves)
13042..13090 more OnFloatValueChanged: 62.68, 65.70, 67.57, 68.73, 70.07, 71.75, 72.74 (one per OnMotionDetected burst)
13092 BndEvt__MainSlider_..._2_OnMouseCaptureEndEvent
13093 Slider_Mobile_C::On Option Changed Final Check  RequiresOptionRefresh=false  PropertyNameToSelect@4=TouchSensitivity
13096 ControlsOptions_C::OptionChanged  TouchSensitivity
13098 OnFloatValueChanged Value=72.738274 (same value; echo/refresh)
13100 Final Check again, 13103 OptionChanged again
```
- The slider value is carried as `Value : FloatProperty @0` of `BndEvt__MainSlider_..._0_OnFloatValueChangedEvent` (e.g. 13021, 13099).
- The commit is `MouseCaptureEnd` followed by `Final Check` and `OptionChanged`.
- The same pattern repeats on the revert drag: 14437-14486, from 60.56 down to 51.95.

**Selection change:** **not captured** (see section 0). By symmetry with Bool: `BndEvt__ForwardButton/BackButton..._OnButtonClickedEvent` then `Final Check` then `OptionChanged`, with the index in `SelectedElement` 0x658. **G.**

**Does the game write into `GameterSettings` by name?**
- **V:** the property name flows through the events as a bare `Name`: `InvertLook` and `TouchSensitivity` (11958, 11963, 11965, 13095, 13097). The same `Name` type is the param of `GetSelectionOptions(PropertyName)` (758), `GrabSpecificOptionFocus(PropertyName)` (9597), and every `OnOptionChange`/`OnOptionSelectionChanged`/`OnFinalConfirmation`/`DidUpdateSettings` delegate.
- **Not V:** the actual write. It happens inside a Blueprint call chain (`UpdateSettings` / `UpdateSettingForConfirmation`) that never appears in EV (section 0).
- **I:** it is by name. The row has no other link to a setting than `Options` plus `SettingsObject` (`InitSlotButton(SettingsObject)`), and the row-to-owner events carry only the `Name`.

## 5. Recipe for rows not backed by real `GameterSettings` properties

### Option A: `AddSlotItem` with a fabricated `Options` struct

| Question | Answer |
|---|---|
| Will it work as a layout path? | **I.** `AddSlotItem` is the only function in the capture that takes row, `Options`, `SettingsObject` together (9611-9613). It probably also wires the row to the list: `ParentScrollingSLot` 0x348, `InteractedSound`, `SlotDispatcher` (so `SlotButtonPressed` fires), alternating colour (uidump4 locals), and the `OnOptionChange`/`OnOptionSelectionChanged` handlers that appear on `ControlsOptions_C`. |
| Will the row read or write a nonexistent property? | **Unknown.** `InitSlotButton` reads `GameterSettings` (locals `GetSettings`, per uidump4). With a nonexistent `PropertyName` the BP property getter probably returns false/default, but no capture line shows it. |
| Is it safe? | **Not verified.** A Blueprint "Accessed None" on a null `SettingsObject` normally logs and continues, but that is general UE4 behaviour, not observed here. |
| `Options` layout | **unknown** (section 1), so you cannot fabricate it blind. Copy a live row's 144 bytes and patch the `FName` fields. |

### Option B: build the row, then fill the widgets directly

- Works on the stable parts: `OverrideMainText`, `OverrideAltText`, `SelectionText`, `SelectedElement`, `SelectionNames`, `MainSlider` (V functions and props above).
- It skips every Options/Settings coupling.
- Cost: you must do the list wiring yourself (`AddChild` into `MainScrollBox`, set `ParentScrollingSLot`, and so on). **G**: `AddSlotItem` binds delegates you would also need.

### Recommendation: hybrid, A for wiring and B for content

Use `AddSlotItem` (A) so the row is wired by the game. Immediately after, overwrite its text and state with B. Intercept the events (below) so the game never applies the change.

Ordered `ProcessEvent` calls. Use `FindFunctionByName` on the class; read param offsets from the `UFunction`.

1. **Find the target list.** **G.** Walk the second `UI_Options_Menu_Tab_SA_Mobile_C` instance (tab 7, mods; constructed at 11402). Pick its `ControlsOptions_C` list (or any of the five). Do not touch the instance for tab 5.
2. **`ClearSlotList` on that list** (`UI_ScrollingSlotList_C`, 9599; no params). V that it exists and has no params. What it clears is **G**. It clears the whole list, which is fine because this is a mods-only instance.
3. **Create a row widget** of the right class. **G:** `CreateWidget(owner, BoolSlotClass...)` with the class read from the template (`SliderSlotClass`, `SelectionSlotClass`, `BoolSlotClass`, `MessageSlotClass`; V 10551-10554). Register the new pointer in your own set.
4. *(Bool/Selection rows only; G.)* Set `IgnoreSettings = true` before step 5: Bool at row+0x65c, Selection at row+0x70c (V that the property exists; the meaning is a guess). The Slider has no such property.
5. **`UI_ScrollingSlotList_C::AddSlotItem`.** Param layout (order V, offsets computed by assuming 8-byte struct alignment, **G**: verify from the `UFunction`):
   - `Slot Item` (`UI_SlotButton_C*`) @0, 8 bytes
   - `Options` (struct, 144 B) @8
   - `SettingsObject` (`GameterSettings*`) @0x98
   - total 0xA0
   - Fill `Options` by copying 144 bytes from a live row of the same class (`row+0x298`) and replacing its `PropertyName`-like `FName` with a unique `GTAML_<id>`. **G:** field offsets unknown until section 6.
   - `SettingsObject`: **G** pass the real object read from a live row's `InitSlotButton` argument (in the next capture), or null.
6. **Set label:** `UI_SlotButton_C::OverrideMainText(NewText: FText @0, 24 B)` (V 840-841). **Set value text:** `OverrideAltText(NewText)` (V 838-839). That Main is the label and Alt is the value is **G**; `Message_C` has `OptionNameText` (left) and `RightText` (right).
   - For Bool/Selection also write `SelectionNames` (`TArray<FText>` row+0x648) and `SelectedElement` (int, row+0x658); then call `UpdateTextColor` / `GetCurrentSettingText` (V exist, no params except the out text) to repaint. Whether `SelectionText` updates automatically is **G**; otherwise call `TextBlock::SetText` on `SelectionText` (row+0x638).
   - For Slider: set `MainSlider` (row+0x5f8) range and value through the UMG `Slider` functions (`SetMinValue`/`SetMaxValue`/`SetValue`, engine functions, **G**), then `UpdateSliderWithValue(Slider*@0, NewValue float@8)` (V 1592-1594) and `UpdateText(InValue)` (V 1613-1614).
7. **Intercept the change.** Hook `ProcessEvent`. If `self` is in your row set (or `OptionName` starts with your prefix), match by function name and **do not call the original** for:
   - Bool/Selection: `BndEvt__ForwardButton_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature` and `BndEvt__BackButton_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature` (V 11956, 15526; no params). **V** that these are the click entry points; **I** that skipping them stops `NextSelection`/`UpdateSettings`, since BP-internal calls cannot be hooked. Cycle `SelectedElement` yourself and repaint (step 6).
   - Slider: `BndEvt__MainSlider_..._0_OnFloatValueChangedEvent__DelegateSignature` (`Value float @0`; V 13021). Read `Value`; skip the original. Commit on `..._2_OnMouseCaptureEndEvent` (V 13092) or `On Option Changed Final Check`.
   - Fallback: `On Option Changed Final Check` (`bool @0`, `Name @4`; V 11961, 13093) and `ControlsOptions_C::OptionChanged` (`Name @0`; V 11964, 13096). Skip both when the name is yours, so the owner never sees a fake name.
   - Controller navigation (`NavigateLeft`/`NavigateRight`, `OnAnalogValueChanged`) was not exercised in the capture. **G:** hook those too.
   - Let hover/focus/press events through unmodified (`OnMotionDetected`, `OnAddedToFocusPath`, `BndEvt..._OnButtonHoverEvent`, `OnOptionSelectionChanged`).
8. **Clear our rows.** To clear everything: `ClearSlotList` on our list (step 2). To remove only some: `Widget::RemoveFromParent` on each row (engine function; **G**). Drop pointers from your set on `Destruct` (V: every row logs `...::Destruct` on teardown, 16452-16695) and when the pause menu closes.

Steps tagged **V** above only mean the function or event exists with those params. **Nothing in steps 3-6 and 8 has been executed or observed**; the recipe is unverified end to end.

## 6. Next capture: the smallest additions that close the gaps

1. On the first `Construct` of each `UI_SlotButton_{Bool,Slider,Selection,Message}_Mobile_C`, dump:
   - hex of `row+0x298 .. +0x328` (144 bytes) with `FName` and `FText` fields resolved,
   - `DumpProps` of the `Options` property's `FStructProperty::Struct`. This gives the field names, types and offsets.
   Do it again after a short delay: the row is fully set up only after `AddSlotItem` and `InitSlotButton`.
2. Dump the `MainSlider` properties (`MinValue`, `MaxValue`, `StepSize`, `Value`) for each slider row. Dump `SelectionNames`, `SelectedElement`, `IgnoreSettings` for each Bool and Selection row.
3. Dump the class of `ControlsOptions_C` and the others (superclass chain, properties, functions), and the `GameterSettings` property list. That tells whether `PropertyName` maps to a real settings property.
4. If you need `AddSlotItem` or `InitSlotButton` argument values, hook the BP function through `UFunction::Func` (the native thunk) instead of `ProcessEvent`. `ProcessEvent` never sees BP-internal calls.
5. Exercise a Selection row (Language or Gameplay) and a controller-navigated row, so those EV sequences are captured.
