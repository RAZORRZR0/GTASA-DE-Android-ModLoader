The cause of `WidgetTree: null or property not found` is that `uidump.cpp` has the FField offsets wrong. `ChildProperties` at 0x50 was already correct. `Next` is 0x20, not 0x18. The name is at 0x28, not 0x20. `Offset_Internal` is 0x4C, not 0x44. `UStruct::FindPropertyByName` is exported, so the mod can call it instead of walking.

I disassembled with Apple `objdump` through `capture_process_scenario`. I decoded the `.data.rel.ro` blobs by hand from `objdump -s`. I did not run anything on the device, and `frida-server` is not running there.

## 1. UStruct layout (libUE4.so, all offsets from disassembly)

| Field | Offset | Evidence |
|---|---|---|
| `SuperStruct` | 0x40 | already known |
| `Children` (UField*) | 0x48 | `TFieldIterator<UField>::IterateToNext` @0x6599f14 does `ldr x8,[x9,#0x48]`, then follows `UField::Next` at +0x28 |
| `ChildProperties` (FField*) | 0x50 | `UStruct::AddCppProperty` @0x65f4be4: `ldr x8,[x0,#0x50]; str x8,[x1,#0x20]; str x1,[x0,#0x50]`. `TFieldIterator<FField>::IterateToNext` @0x6670b1c uses `[x9,#0x50]` and `Next=+0x20`. `DestroyChildPropertiesAndResetPropertyLinks` @0x65f771c walks 0x50 via +0x20 |
| `PropertiesSize` (int32) | 0x58 | `UStruct::InitializeStruct` @0x65f609c: `ldp w8,w9,[x0,#0x58]`, then the size is rounded up to the alignment in 0x5C |
| `MinAlignment` (int32) | 0x5C | same instruction |
| `PropertyLink` | 0x70 | `FindPropertyByName`: `ldr x0,[x0,#0x70]`. `InitializeStruct` iterates it. `DestroyChildProperties…` clears 0x70–0x8F with `stp q0,q0,[x19,#0x70]`. |
| `RefLink`, `DestructorLink`, `PostConstructLink` | 0x78, 0x80, 0x88 | same 0x70–0x8F clear |
| `UClass::FuncMap` | 0x138 | already known |
| `UClass::Interfaces` | data 0x208, num 0x210, 16-byte elements | the iterator reads `[x0,#0x208]` and `[x0,#0x210]` with `lsl #4` |

## 2. FField / FProperty / FFieldClass

**FField**

| Field | Offset | Evidence |
|---|---|---|
| vtable | 0x00 | |
| `ClassPrivate` (FFieldClass*) | 0x08 | iterators do `ldr x9,[x8,#0x8]; ldrb [x9,#0x10]` |
| `Owner` (container ptr) | 0x10 | the `FProperty` ctor @0x6707df0 does `stp x1,x2,[x0,#0x10]` |
| `bIsUObject` (byte) | 0x18 | `ldrb w9,[x19,#0x18]` at 0x6707ed0 |
| `Next` | 0x20 | `AddCppProperty`: `str x8,[x1,#0x20]` |
| `NamePrivate` (FName, 8 bytes) | 0x28 | ctor: `ldr x8,[x3]; str x8,[x0,#0x28]`; `FindPropertyByName` compares `[x0,#0x28]` with the 8-byte FName |
| `FlagsPrivate` (u32) | 0x30 | ctor: `str w4,[x0,#0x30]` |

`FField` is 0x34 bytes, so `FProperty` members start at 0x34.

**FProperty**

| Field | Offset | Evidence |
|---|---|---|
| `ArrayDim` (int32) | 0x34 | ctor `stur d0,[x0,#0x34]` stores ArrayDim=1 and ElementSize=0. `InitializeStruct` computes `ldp w9,w10,[x21,#0x34]; madd w9,w10,w9,w8` |
| `ElementSize` (int32) | 0x38 | same |
| `PropertyFlags` (u64) | 0x40 | ctor `str xzr,[x0,#0x40]`; `ldrb [x21,#0x41]` bit1 = `CPF_ZeroConstructor` |
| `RepIndex` (u16), `BlueprintReplicationCondition` (u8) | 0x48, 0x4A | ctor stores |
| `Offset_Internal` (int32) | 0x4C | `InitializeStruct`: `ldr w8,[x21,#0x4c]`; ctor `str w5,[x0,#0x4c]` |
| `RepNotifyFunc` (FName) | 0x50 | ctor zeroes 0x50–0x6F |
| `PropertyLinkNext` | 0x58 | `FindPropertyByName`: `ldr x0,[x0,#0x58]` |
| `NextRef`, `DestructorLinkNext`, `PostConstructLinkNext` | 0x60, 0x68, 0x70 | ctor stores |
| sizeof(FProperty) | 0x78 | subclass members start at 0x78 |

**Subclasses**
- `FStructProperty::Struct` is at 0x78 (ctor @0x675ca08 does `str xzr,[x0,#0x78]`).
- `FArrayProperty::Inner` is at 0x78 (`GetInnerFieldByName` @0x670feb4 and `GetInnerFields` read `[x0,#0x78]`).
- `FObjectPropertyBase::PropertyClass` is at 0x78 (`AddReferencedObjects` @0x6710f68 passes `x0+0x78` to the collector).
- `FByteProperty::Enum` is at 0x78. `FEnumProperty` has `UnderlyingProp` at 0x78 and `Enum` at 0x80 (from `PropertyAccessUtil::GetPropertyEnumType` @0x670b550).
- `FBoolProperty` byte fields (`FieldSize`, `ByteOffset`, `ByteMask`, `FieldMask`) are probably at 0x78..0x7B. [INFERENCE: stock 4.26 layout; the ctor at 0x6728a28 did not show those stores.]

**FFieldClass** (ctor @0x667d878)
- `Name` (FName) is at 0x00.
- `Id` is at 0x08 and `CastFlags` at 0x10.
- `ClassFlags` is at 0x18 (u32).
- `SuperClass` is at 0x20, `DefaultObject` at 0x28, `ConstructFn` at 0x30.
- Read the type name as `FName(*(u64*)(*(void**)(prop+0x08)))`. It has no `F` prefix: "ObjectProperty", "ClassProperty", "StructProperty", "ArrayProperty", "TextProperty", "StrProperty".

**Corrected constants for `uidump.cpp`:** `Field_Next` 0x18→**0x20**, `Field_Name` 0x20→**0x28**, `Prop_Offset` 0x44→**0x4C**. `Struct_ChildProps = 0x50` was right. Also compare the full 8-byte FName (index and number), as the engine does.

## 3. `UStruct::FindPropertyByName` is exported

- Mangled name: `_ZNK7UStruct18FindPropertyByNameE5FName`, at 0x670b530.
- Signature: `FProperty* (const UStruct* this /*x0*/, FName /*x1, 8 bytes by value*/)`.

Full body:
```
ldr x0,[x0,#0x70]        ; PropertyLink
cbz x0,ret
L: ldr x8,[x0,#0x28]     ; FField::NamePrivate
   cmp x8,x1 ; b.eq ret
   ldr x0,[x0,#0x58]     ; PropertyLinkNext
   cbnz x0,L
ret
```

- Call it on the object's class: `FProperty* p = FindPropertyByName(*(UClass**)(obj+0x10), fname)`.
- The value address is `obj + *(int32*)(p+0x4C)`.
- It only searches `PropertyLink`, which is built at `Link()` time. Inherited properties are included only if `PropertyLink` chains into the super's, which is stock UE behaviour. [INFERENCE: I did not disassemble `UStruct::Link`.]
- `PropertyAccessUtil::FindPropertyByName(FName, UStruct const*)` @0x670c694 is also exported.
- Other useful exports:
  - `FName(const char*, EFindName)`: `_ZN5FNameC1EPKc9EFindName`.
  - `FMemory::Malloc(unsigned long long,unsigned)` @0x62b6534, `Realloc` @0x62b6778 and `Free` @0x62b6698, mangled `_ZN7FMemory6MallocEyj`, `…7ReallocEPvyj` and `…4FreeEPv`.
  - `FText::FromString(FString const&)` @0x63263dc.
  - `UWidget::GetParent() const` @0x836d780.
  - `UPanelWidget::AddChild` @0x8331554.

## 4. Native property tables

Format: each `NewProp_*` static is `FPropertyParams`. Decoded layout from the bytes:
- +0x00 NameUTF8 (a relocated pointer, so zero in the file) and +0x08 RepNotify.
- +0x10 `PropertyFlags` (u64).
- +0x18 `EPropertyGenFlags` (u8; 3=Int, 0x0A=Float, 0x0D=SoftClass, 0x11=Class/TSubclassOf, 0x12=Object, 0x15=Str, 0x16=Array, 0x19=Struct, 0x1D=Text).
- +0x1C `ObjectFlags` (always 0x45).
- +0x20 `ArrayDim`.
- +0x24 `Offset` (u16).
- Object, Class and Struct entries add a function pointer at +0x28 (0x30 bytes total); scalars are 0x28.
- Array `_Inner` entries have `Offset=0`.

I decoded 61 properties of `UGTAMainMenuUITemplate`. Its first property is at 0x30, which suggests a `UDataAsset`-style parent (0x28 + `NativeClass`) [INFERENCE].

**UGTAMainMenuUITemplate** (offset: name, type)

| Offset | Property | Type |
|---|---|---|
| 0x30 | GameBitMask | int32 |
| 0x38 | BackgroundWidget | TSubclassOf |
| 0x40 | GameIcon | struct (0x88, probably FSlateBrush) |
| 0xC8, 0xD8, 0xE8, 0xF8, 0x108, 0x118, 0x128, 0x138, 0x148, 0x158 | MainButtonsAnchors, MainButtonsOffsets, TabAreaAnchors, TabAreaOffsets, LogoAreaAnchors, LeftPanelsAnchors, LeftPanelsOffsets, MapKeyAnchors, MapNavigationAnchors, MapAreaAnchors | struct (0x10 each) |
| 0x168 | KeyEntryOverride | TSubclassOf |
| 0x170 | MapFontOverride | struct (0x58) |
| **0x1C8** | **LeftButtonTemplate** | TSubclassOf |
| **0x1D0** | **LeftButtons** | TArray<FText> |
| **0x1E0** | **AltLeftButtonText** | TArray<FText> |
| 0x1F0 | LeftButtonSelectedBackgroundOverride | TArray<UObject*> |
| 0x200 | LeftButtonVerticalSize | float |
| 0x204 | HelpTextShadowColor | struct (0x10) |
| 0x218, 0x220, 0x228, 0x230, 0x238, 0x240 | StartupSound, ExitSound, StartSound, BackSound, MainButtonHighlightSound, TabHighlightSound | UObject* |
| 0x248 | SelectorTabs | TArray<TSubclassOf> |
| **0x258** | **FunctionToCallOnPress** | TArray<FString> |
| 0x268, 0x26C | StartingTab, SaveGameTab | int32 |
| 0x270, 0x3C8 | SlotStyleOverride, SlotStyleDefaults | struct (0x158) |
| 0x520 | AdjustableControlsLayout | TSubclassOf |
| 0x528, 0x680, 0x708 | MapKeyStyle, MapKeySlotBackgroundBrush, MapHelpBackground | struct |
| 0x980, 0x988, 0x990, 0x998, 0x9A0 | SliderSlotClass, SelectionSlotClass, BoolSlotClass, MessageSlotClass, SaveGameSlotClass | TSubclassOf |
| 0x9A8, 0x9B0, 0x9B8, 0x9C0 | TabConfirmationDialog, FullScreenConfirmationClass, ControllerDisconnectClass, MissionRetryConfirmationClass | TSubclassOf |
| 0x9C8 | RadioVolumeMultiplier | float |
| 0x9D0 | RadioStations | TArray<UObject*> |
| 0x9E0, 0x9E8 | DialogSampleSound, SFXSampleSound | UObject* |
| 0x9F0 | SCBackgroundTint | struct |
| 0xA18, 0xA20 | SCBackgroundWidget, SCButtonTemplate | TSubclassOf |
| 0xA28 | PreMenuLegalScreen | TSoftClassPtr |
| 0xA50 | LoadingScreen | TSubclassOf |

- **Menu-entry struct:** none exists in `nm_dem`. The only struct-typed properties are engine structs (anchors, margins, brushes, styles). I did not resolve which engine struct each is; sizes suggest FAnchors, FMargin, FSlateBrush, FSlateFontInfo, FLinearColor. The template ships no "menu item" type, only parallel arrays.
- **Element sizes:** read them at runtime from `*(int32*)(Inner+0x38)` rather than assuming. Inner = `*(FProperty**)(arrayProp+0x78)`. FText is probably 0x18 [INFERENCE].

**UUI_Menu_Base**: only `Gameterface` (UGameterface*) at **0x290**. It is the last member: `CreateNewMenu` caches the class with size 0x298 (`mov w4,#0x298`). Natives are `SetTemplate`, `CustomFunctionFromString(FString const&)`, `CustomFunctionFromStringWithText(FString const&, FText const&)`, `CustomFunctionWithBoolReturn(FString const&, bool&)`, `GrabFocus`, `StartMenuSFX` and similar. These are thin Blueprint-event thunks, so the logic lives in the Blueprint.

**Related classes**
- `UGTAOptionsList` has `Category` at 0x30 (byte enum) and `Options` at 0x38 (TArray of struct). It also has no usable behaviour.
- `UUI_LoadingScreen` has `Gameterface`.
- **AGTAWorldSettings** (class size 0x528):

| Property | Offset |
|---|---|
| MainMenuUITemplate | 0x4A0 |
| MainMenuClass | 0x4A8 |
| PauseMenuUITemplate | 0x4B0 |
| PauseMenuClass | 0x4B8 |
| SaveMenuClass | 0x4C0 |
| MobileMainMenuUITemplate | 0x4C8 |
| MobilePauseMenuUITemplate | 0x4D0 |

## 5. Injection recommendation

**Strongest candidate:** `LeftButtons` (FText) with `FunctionToCallOnPress` (FString) are parallel arrays, and the template also carries `LeftButtonTemplate` (a widget class). That looks like the data that builds the left-hand button list at runtime. Your dump matches: the buttons are created with outer=GameInstance, a click goes to `InteractedButton`, and `UUI_Menu_Base::CustomFunctionFromString` takes a function-name string. I could not confirm this natively, because the logic is in the `UI_PauseMenu_C` Blueprint [INFERENCE].

**Plan A (preferred): append to the template data before the menu builds its buttons.**
1. **Confirm first.** On pause-menu open, read the template at `AGTAWorldSettings+0x4B0` (`PauseMenuUITemplate`) or `+0x4D0` (`MobilePauseMenuUITemplate`; the mobile one is likely the live one). Log the contents of `LeftButtons` (+0x1D0) and `FunctionToCallOnPress` (+0x258). If they hold the Resume/Map/Brief/Stats/Options strings, Plan A is right.
2. **Hook point.** Hook `UGameterface::CreateNewMenu`, mangled `_ZN12UGameterface13CreateNewMenuE11TSubclassOfI13UUI_Menu_BaseEP22UGTAMainMenuUITemplateib` @0x4b1b27c, where `x2` is the template. Or hook `ProcessEvent` on `SetTemplate`, whose parms are probably `{template*, bool}` [INFERENCE]. `StartPauseMenu` @0x4b1cafc fetches `AGTAWorldSettings` and returns early if `this+0x368` is set; I did not read the rest of it.
3. **Mutation.** Append to each parallel array once, guarded because the template is a shared data asset that persists across menu opens. Grow with `FMemory::Realloc` and bump both Num and Max. Fill the new FText from `FText::FromString` and the new FString with an engine-allocated buffer. Also keep `AltLeftButtonText` (0x1E0) and `LeftButtonSelectedBackgroundOverride` (0x1F0) in step if their lengths match `LeftButtons`.
4. **Click handling.** On `ProcessEvent` for `InteractedButton`, or the `CustomFunctionFromString` call, check whether the string equals your function name. If so, open your screen and skip the original.

**Plan B (fallback): create a `UI_Button_Menu_SA_C` yourself.**
1. **Class lookup.** Find the class with `StaticFindObject` on its asset path or a `GUObjectArray` scan.
2. **Construction.** Create it with `UUserWidget::CreateWidgetInstance` (`…ER6UWorld…` @0x83d48c0 or `…ER17APlayerController…` @0x83d3e0c), then set its label.
3. **Parent panel.** Find an existing button's parent with `UWidget::GetParent()` @0x836d780 (or the `Slot`→`Parent` properties). Add yours with `UPanelWidget::AddChild` @0x8331554.
4. **Click handling.** Hook `ProcessEvent` for the `BndEvt__MainButton_…OnButtonClickedEvent` event on your button, as your dump shows.
5. **Drawback.** The menu's own focus and navigation code does not know about the extra button.

## Files
- `/tmp/gtamod/repo/mods/uidump/uidump.cpp:27-34, 163-187`: the wrong `Field_Next`, `Field_Name` and `Prop_Offset` constants and the walker that uses them.
- `/tmp/gtamod/nm_dem.txt` and `nm_mangled.txt`: all symbol addresses above.
- `/tmp/gtamod/menu_research.md`: earlier research (the exported UMG natives).
- `/tmp/gtamod/repo/docs/arm64_map.md`: existing offset table; add the values above.

**Not checked:**
- I did not disassemble `UStruct::Link` or the pause-menu Blueprint, so the "template drives the button list" claim needs the runtime log in step 1.
- I did not resolve which engine struct each struct property is.
- The `FText` size and `SetTemplate` parms layout are stock-4.26 assumptions.

**Risk:** the template is a shared asset, so a bad array resize or double append persists until the app restarts.