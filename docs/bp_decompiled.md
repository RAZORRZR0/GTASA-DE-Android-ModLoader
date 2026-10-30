# Options UI Blueprint bytecode (GTA SA DE Android, v1.87.0, UE 4.26)

Source: cooked Blueprint bytecode (`.uasset`/`.uexp`), read with kismet-analyzer.
Output is bytecode listings (`EX_*` per block), not pseudocode. Line numbers are listing lines in `/tmp/gtamod/bp/txt2/<Name>.txt`. Nothing here is confirmed at runtime.
Items marked [UNVERIFIED] were not checked against the listing.

## Pipeline

| Tool | Version / source |
|---|---|
| repak_cli | 0.2.3, cargo; binary `~/.cargo/bin/repak` (`PATH=$HOME/.cargo/bin:$PATH`) |
| .NET SDK | 10.0.403, `brew install dotnet` |
| kismet-analyzer | `/tmp/gtamod/bp/kismet-analyzer`, commit `e8982e9`, net9.0; UAssetAPI commit `6c2959be` (from an earlier session, not re-checked) |

```
export DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec DOTNET_ROLL_FORWARD=Major
cd /tmp/gtamod/bp/kismet-analyzer && dotnet build -c Release
./bin/Release/net9.0/kismet-analyzer gen-cfg <Asset.uasset> /tmp/gtamod/bp/txt2
```
`gen-cfg` writes `<Name>.txt` (listing) and `<Name>.dot`. Some blocks log `WARN: Unimplemented non-EX_IntConst`, so branch conditions may be missing.

Paks: v11, zlib only, footer magic `0x5A6F12E1`. No AES key was needed.

Pak map (verified by `grep -l` against `/tmp/gtamod/paks/list_*.txt`):

| Asset | Pak |
|---|---|
| `Common/UI/Buttons/SlotTemplates/UI_SlotButton_{Bool,Selection,Slider}_Mobile` | `pakchunk0_s37.pak` |
| `Common/UI/Buttons/SlotTemplates/UI_SlotButton_Bool`, `Buttons/UI_SlotButton`, `Tabs/UI_TabbedLayout` | OBB `pakchunk1-Android_ASTC.pak` |
| `UI_Button_Base`, `Tabs/Options/UI_OptionsMenuTab` | [UNVERIFIED] |

The OBB is `base.apk` entry `assets/main.obb.png` (stored zip) holding `pakchunk1-Android_ASTC.pak`, extracted to `/tmp/gtamod/paks/obb/`. The partial `pakchunk0-Android_ASTC.pak` was removed from there and is gone. Unpacked trees: `/tmp/gtamod/paks/unpacked_obb1/Gameface/Content/` and `/tmp/gtamod/paks/unpacked/Gameface/Content/`.

## Slider rows (`UI_SlotButton_Slider`)

Init, `InitSlotButton` (export at line 372):
```
Slider->SetMinValue(Options.MinSlider)                                      // line 285
Slider->SetMaxValue(Options.MaxSlider)                                      // line 335
if (Options.StepSizeOverride > 0) Slider->SetStepSize(Options.StepSizeOverride)  // line 465
value = GameterSettings->GetFloatValue(Options.PropertyName)                // line 207
UpdateSlider(value)                                                         // line 540: MainSlider->SetValue(v)
```

Events:
- `OnFloatValueChanged` (drag): if `UseText`, `UpdateText`. Calls `HasBeenSelected`. Calls `UpdateSettings(False)` and `OnOptionChange` only when `!bIsMouseCaptured`. Drag alone does not write.
- `OnMouseCaptureBegin` (1301): sets `bIsMouseCaptured`.
- `OnMouseCaptureEnd` (1246): clears it, calls `UpdateSettings(False)`, broadcasts `OnOptionChange`. This is the drag commit.
- `OnEditableTextBoxCommitted` (966): broadcasts `OnOptionChange`, then `UpdateSettings(False)`.
- `NavigateLeft` (851) / `NavigateRight` (889): step, `FClamp` to `MainSlider.MinValue`/`MaxValue`, then `UpdateSliderWithValue`.
- `UpdateSliderWithValue` (927): `Slider->SetValue`, `UpdateText` if `UseText`, `UpdateSettings(False)` (writes), `HasBeenSelected`, `OnOptionChange`.

Write path, `UpdateSettings(bool)` (551):
```
if (!RequireConfirmationOnChange || OverrideConfirmationLockout)
    SetFloatValue(PropertyName, UseSlider ? MainSlider.GetValue() : parse(EditableTextBox))
    broadcast DidUpdateSettings
else
    UpdateSettingForConfirmation
```

Show value V in [A,B] without writing GameterSettings: `V' = clamp(V, A, B)` and call `UpdateSlider(V')` (`MainSlider->SetValue`). Do not call `UpdateSliderWithValue`, `NavigateLeft/Right`, or `UpdateSettings`, since they write.
[INFERENCE] `USlider::SetValue` may broadcast `OnValueChanged`, which reaches `OnFloatValueChanged`. That path writes only when not captured, so it could still write. Check with a frida hook on `UGameterSettings::SetFloatValue` (0x4b5014c).

`UI_SlotButton_Slider_Mobile` forwards to `/Game/Common/UI/Buttons/SlotTemplates/UI_SlotButton_Slider_C`.

## Bool and Selection rows

Base `UI_SlotButton`: `On Option Changed Final Check` calls `GameterSettings->FinalOptionCheck(PropertyName, Options)`. Cancelled confirmation calls `RevertOption`. `IsSettingAvailable` and `TickDisabled` call `CheckSpecialOptionEnabled`. `IsSettingAvailable` calls `FixDisplayName`, then `OptionNameText.SetText`.

`UI_SlotButton_Bool` (`UI_SlotButton_Bool.txt`):
- `InitSlotButton` (1392): `FixDisplayName` (1421), `OptionNameText.SetText` (1426), `GetBoolValue` (1466).
- `UpdateSettings` (export 1125): `SetBoolValue(PropertyName, SelectedElement > 0)`, `SetBoolValue` at 1184 (`Greater_IntInt`).
- `UpdateSettingForConfirmation` (646): `GetBoolValue` at 676, `SetHasChangedSinceLastSave` at 689. No setter.
- `InitSelectionStyle` (692): writes only `OptionNameText.ColorAndOpacity` (719, 735).
- Ubergraph: `Construct` call at 892, then `InitSelectionStyle` at 902. `OnSynchronizeProperties` call at 863, then `InitSelectionStyle` at 873.
- `NextSelection` 758, `PreviousSelection` 870, `ToNumSelection` 1004, `UpdateBGColor` 1077, `UpdateTextColor` 1235.
- `Tick` (609): [UNVERIFIED] what block 830 calls.

`UI_SlotButton_Selection` (`UI_SlotButton_Selection.txt`):
- Ubergraph: `Construct` call at 843, then `InitSelectionStyle` at 853. `OnSynchronizeProperties` call at 872, then `InitSelectionStyle` at 882.
- `UpdateSettings` (export 1150), verified this session:
  ```
  block 5:   if (Not_PreBool(RequireConfirmationOnChange)) -> 331 (UpdateSettingForConfirmation) else -> 95
  block 95:  push 346 -> broadcast DidUpdateSettings(RequireListRefreshOnChange, PropertyName)
  block 162: if (EqualEqual_ByteByte(Options.Style, 0)) -> 216 else -> 402
  block 216: SetIntValue(PropertyName, Add_IntInt(SelectedElement, Options.SelectionOffset))   // line 293 call
  block 402: SetBoolValue(PropertyName, Greater_IntInt(SelectedElement, 0))                    // line 458 call
  ```
- [UNVERIFIED] Selection `InitSlotButton` (723), `FixDisplayName` (752), `OptionNameText.SetText` (757), `GetIntValue` (833), `Subtract_IntInt` (838), `GetBoolValue` (878). The names are in the listing but the line numbers were not re-checked.
- [UNVERIFIED] `UpdateSettingForConfirmation`, `ToNumSelection`, `NextSelection`, `PreviousSelection`, and `InitSelectionStyle` export line numbers. Earlier values matched Bool exports, so they are likely wrong.
- `Tick`: not read.

## Text resets (what sets `OptionNameText` / `SelectionText`)

- `OptionNameText.SetText`: observed in Bool `InitSlotButton` (1426), and in base `IsSettingAvailable` via `FixDisplayName`. [UNVERIFIED] Slider and Selection sites.
- `InitSelectionStyle` changes only `ColorAndOpacity`. `UpdateTextColor` changes color. `SetFont` changes font. None of these set text.
- `SelectionText.SetText`: [UNVERIFIED] in `ToNumSelection`, which is called from Bool `InitSlotButton` (1447) and Selection `InitSlotButton`.
- Consequence: `OptionNameText` is rewritten only when `InitSlotButton` runs again. `SelectionText` is rewritten on each value change through `ToNumSelection` [UNVERIFIED].
- Native rename: call `OptionNameText->SetText(FText)` after `Construct`. It stays until `InitSlotButton` runs again [INFERENCE: whether a list refresh re-runs it is not verified].

## Sub-tab headers ("controls", "gameplay", "graphics", "audio", "accessibility")

Labels are in `UI_OptionsMenuTab.uexp`. Each label follows a 32-hex key, in byte order:

| Key | Text |
|---|---|
| `5AA9E4514E1983AEF76743AE1F0D0953` | `controls` |
| `60E3573C4B20414D564BFDB1DB713F10` | `gameplay` |
| `D724B3CE4A9963AF5C3628856F63F8ED` | `graphics` |
| `993D870840385852E8A1F583CE704DBA` | `audio` |
| `4B81E4CA42CD978A270E8A8AD9E99D6B` | `accessibility` |

[INFERENCE] The key→text pairing comes from byte order, not a decoded FText.

`UI_OptionsMenuTab.txt`: a grep for `TabNames`, `TabStyle`, and `SetButtonTextValue` returned no matches. So the labels are not written in that bytecode. They are most likely a CDO default of `TabStyle.TabNames` stored in the uexp. [INFERENCE]

Build chain, `UI_TabbedLayout.txt`:
```
SetupTabs (240)   reads TabStyle.TabNames[i] via KismetArrayLibrary->Array_Get (line 1368), calls AddTab (1414)
AddTab (export 945)  SetButtonTextValue(TabText, False) (line 247); adds to TabButtons (line 303)
```
`ResetTabs` (2236): loops over `TabStyle.TabNames` length (`Array_Length`, block 87). [UNVERIFIED] whether it calls `AddTab`.

`UI_Button_Base` (bytecode observed, listing line numbers not checked):
```
SetButtonTextValue(Value, JustRebuild):
  if (!JustRebuild) ButtonTextValue = Value
  ButtonText.SetText(ButtonTextValue)
```
Its `Construct` calls `SetButtonTextValue(<literal>, True)`, so the literal is ignored and the stored value is re-applied.

Rename from native code after `Construct`: for each `TabButtons` child, cast to `UI_Button_Base_C` and call `SetButtonTextValue(FText, false)`. `true` does not change the stored text. Calling `ButtonText->SetText` directly is simpler, but may be overwritten by a later rebuild [UNVERIFIED].

## Other findings

- `UI_SlotButton_*_Mobile` forwards to `SlotTemplates/UI_SlotButton_*_C`.
- `AudioOptions::OptionChanged` calls the base hook, then `UpdateSounds`. `UpdateSounds` writes no settings. Options: `MusicVolume`, `SFXVolume`, `DialogueVolume`, `DefaultRadioStationSA/VC/III`.
- `UI_OptionsList_Base`: `OptionChanged`, `OnOptionSelectionChanged`, `Option Pressed Callback` are empty hooks.
- `UI_OptionsMenuTab`: `SetToTab` forwards to `UI_TabbedLayout_C.SetToTab`, then fires `ConfirmationRequired`. [UNVERIFIED] `PrevTab`, `NextTab` bodies.
- `UI_OptionsMenuTab` instance vars: `WidgetSelector`, `TabButtons`, `MainButton`, `MainScrollBox`, `AdjustableControlsClass`.
- Native `UGameterSettings` symbols (`nm_dem.txt`): `SetIntValue` 0x4b50b9c, `SetBoolValue` 0x4b50f14, `SetFloatValue` 0x4b5014c, `GetIntValue` 0x4b5049c, `GetBoolValue` 0x4b50858, `GetFloatValue` 0x4b4fe2c, `RevertOption` 0x4b574dc, `FinalOptionCheck` 0x4b570a4, `CheckSpecialOptionEnabled` 0x4b546d8.
- `Z_Construct_UScriptStruct_FGTAOptionsItemTemplate` at 0x4d32cbc. Field offsets not computed.

## Not done

- Key remap (`AdjustableControls`).
- `ControlsOptions`, `LanguageOptions`, `GraphicsOptions`, `GameplayOptions`, `UI_OptionsList_Game`: partly read.
- Every [UNVERIFIED] item above.
- Runtime confirmation. Frida hooks on the `UGameterSettings` setters are the next step.

## GameterSettings unknown names

Source: disassembly of `libUE4.so` with llvm-objdump 18 (NDK 27.3). Function bounds come from the next exported symbol's start address, so they are inferred. Files: `/tmp/gtamod/dis_*.txt`.

Lookup mechanism (all Get/Set/Check functions):
- `FName` is compared by 64-bit equality (`cmp x9, x20`) with `[node+0x28]`.
- No `FindPropertyByName`, `FindField`, or map call appears. The code walks a linked list: head at `[struct+0x70]`, next at `[node+0x58]`, loop until `cbz`/`cbnz` ends on null.
- The searched structs are cached `GetStaticStruct` results held in `x22`-relative globals. The lookup repeats across five or more such structs in sequence.
- A match reads a per-struct base inside `this` (offsets `+0x12c`, `+0x1ac`, `+0x20c`, `+0x228`, `+0x248`), plus `[node+0x4c]` (ldrsw offset).
- Type check: a flag byte of the field's type object (`[[node+0x8]+0x12]` bit 1 for bool and int; `+0x11` bit 0 for float). The semantics of those bits are [UNVERIFIED].
- The only `bl`s on the lookup path are `GetStaticStruct` on the cache miss, and `ConstructUPackage` at first use. Nothing asserts or throws.

Miss behavior (name not found, or type mismatch):
- `GetBoolValue` (0x4b50858): falls to `mov w0, wzr` and returns `false`.
- `GetIntValue` (0x4b5049c): `mov w0, wzr` at 0x4b507b4, returns `0`.
- `GetFloatValue` (0x4b4fe2c): `fmov s0, #-1.0` at 0x4b500e4 is the default, returns `-1.0`.
- `SetBoolValue` (0x4b50f14), `SetIntValue` (0x4b50b9c), `SetFloatValue` (0x4b5014c): all miss paths branch to the epilogue and return `void`. No write, no call.

Hit behavior for the setters:
- `SetFloatValue`: write `str s8,[x8]` only if the value changed (`fcmp`/`b.eq`). Then tail-calls `b 0x4b4f51c` (`GTASaveSettings(FName)`, with `x0=this`, `x1=FName`).
- `SetBoolValue`: writes through a bitmask (`tst` and `bic`/`orr`) and tail-calls `0x4b4f51c` on the same path.
- `SetIntValue`: `str w19,[x8,x9]` then tail-calls `0x4b4f51c`.
- So a real name that is set to a changed value also saves settings. An unknown name does neither.

`CheckSpecialOptionEnabled` (0x4b546d8):
- Zeroes its out-int (`str wzr,[x2]`).
- Compares the FName argument with two hard-coded names built by `0x64f4c90` (FName from string literal). One branch calls a virtual at `[vtable+0x2f8]` on the global at `0xb0b3000+0x4e8` (its `[x+0x228]` subobject), and returns `&1`. The other returns `this->[0x184] == 0`. Another returns `this->[0x198] != 0`.
- Any other name falls through to `mov w0, wzr`, returning `false`.

`FinalOptionCheck` (0x4b570a4):
- Its generic body runs for every name. It calls the virtual at `[vtable+0x280]`, reads a value from `[global+0x4e8]+0x228`, and calls `[vtable+0x2b0]` on `this`, plus other calls [UNVERIFIED].
- Name-specific branches are compared with `0x64f4c90` literals at offsets `0x854`, `0x863`, `0xb88`, `0x2d`. None matches an unknown name.
- It does not assert on unknown input.

Verdict for `"GTAML_Dummy"`:
- SAFE in the sense that matters. No assert, check, or null dereference is reachable on a miss. The lookup is a linear walk with null tests.
- `GetBoolValue` returns `false`, `GetIntValue` returns `0`, `GetFloatValue` returns `-1.0`.
- `SetXValue` does nothing and does not call `GTASaveSettings`. `CheckSpecialOptionEnabled` returns `false`.
- The default for `FinalOptionCheck` is the generic tail, which runs with `this` only. Its vtable callees are [UNVERIFIED], so do not rely on it.
- Caveats: the FName equality check uses the full 64-bit FName, so the number part matters. Create the dummy with `FName("GTAML_Dummy")` once and reuse it. The struct walk assumes the cache pointers at `x22` are populated, which the first `GetStaticStruct` call does.
