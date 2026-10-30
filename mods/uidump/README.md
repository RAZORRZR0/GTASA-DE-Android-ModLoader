# uidump

Read-only diagnostic AML mod for GTA SA Definitive Edition (UE 4.26, arm64). It logs UMG widget activity so you can see the pause/menu widget classes and their widget tree. It does not change gameplay: every `UObject::ProcessEvent` call is forwarded to the original.

## Build

    sh mods/build.sh        # builds hello, propfix, uidump
    # or only this mod:
    cd mods/uidump && $NDK_BUILD NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-23 APP_ABI=arm64-v8a

Output: `mods/uidump/libs/arm64-v8a/libuidump.so`. Deploy to `Android/data/com.rockstargames.gtasa.de/files/mods/`.

## Read the dump

    adb logcat -s UIDUMP

Two kinds of lines:

1. **Class/function pair** (once per unique pair):

       <Class> :: <Function> obj=<UObject*> outer=<Outer1> <- <Outer2> ...

   Logged only when the object's class or any SuperStruct name contains `Menu`, `Pause`, `Frontend`, `Button` or `WBP_`. The `outer` chain shows where the widget lives in the UObject hierarchy (nearest outer first, up to 8 hops).

2. **Widget tree** (once per class, on the first `Construct` of `UI_PauseMenu_C` or `UI_MainMenu_C`):

       == widget tree of <Class> @<obj>
       <Class> '<name>' @<ptr>             depth 0 = WidgetTree.RootWidget
         slot[0]
           <Class> '<name>' @<ptr>         depth 1 (child of a panel)
       == end of tree

   Indentation is 2 spaces per depth level. `slot[i]` is an entry in `UPanelWidget::Slots`. Child nodes are read through `UPanelSlot::Content`. Other special lines: `(null)`, `<not a live UObject @...>`, `... (depth cap)`.

3. **Class reflection** (`CLS` lines, once per class):

       CLS class <Class> @<ptr>                  one class, then its super chain
       CLS   chain[i] <Class>                    SuperStruct walk, stops at UUserWidget/UObject
       CLS <Class>.<Prop> : <Type> [inner=...|class=...|struct=...] off=0x.. elem=..
       CLS <Class> fn <Function>                 UFunction in the Children chain
       CLS <Class>.<Fn> param <Name> : <Type> [Parm|OutParm|ReturnParm]

   Triggers: `UI_ScrollingSlotList_C` (any ProcessEvent on it), `UI_PauseMenu_C` (on `Construct`), and the `GTASA_PauseMenu_Mobile` template on `SetTemplate`. The template dump also logs `CLS SelectorTabs[i] -> <class>` and `CLS template.<Slot> -> <class>` for `SliderSlotClass`, `SelectionSlotClass`, `BoolSlotClass`, `MessageSlotClass` and `LeftButtonTemplate`.

   Properties are read from `UStruct::ChildProperties` (0x50). Each class body is logged once per process; classes already logged appear only as names in the chain.

4. **Option button instance values** (`CLS inst` lines, once per class, after `Construct` returns on `UI_OptionButton_*_Mobile_C`):

       CLS inst == <Class> @<obj>
       CLS inst <Class>.<Prop> = <value>

   Int, float, text, string, object and class values are read directly. Bool values use the byte at `FBoolProperty` +0x79 and mask at +0x7B. These two offsets are inferred, not runtime-verified; check the output against a known toggle state before relying on them.

## Limits

- Max 6000 `UIDUMP` lines per process. Output after that is dropped silently.
- `CLS` walks are capped at 512 members per class and 256 classes per process.
- Tree depth capped at 12. At most 256 slots per panel.
- Only panel children are followed. Single-child content widgets (for example Border, Button) show only their tree node, not their `Content`.
- Offsets are reflection-based: `Gameterface`, `LeftButtons` and the other properties in the layout are found by name through each class's `SuperStruct` chain (`UStruct::FindPropertyByName`). Reflected offsets are authoritative; a documented-offset mismatch logs one `MISMATCH` line per name. Verified in docs/reflection_layout.md (UE 4.26): UStruct SuperStruct 0x40, FProperty Offset_Internal 0x4C.
- Pointers are read with `gtade::SafeRead`. Object pointers are validated against GUObjectArray (chunked) before use.
- Menu template: on `SetTemplate` of a `UI_Menu_Base` class, the button template (`LeftButtonTemplate`, `LeftButtons`, `AltLeftButtonText`, `LeftButtonVerticalSize`, `SelectorTabs`, `FunctionToCallOnPress`) is logged as `== template` lines.
- Custom functions (`CustomFunctionFromString`, `CustomFunctionFromStringWithText`, `CustomFunctionWithBoolReturn`) log their first FString parm and the Gameterface state. `InteractedButton` logs its first 32 parm bytes as hex.
- Risk: `FText::ToString` is called on a value that has only passed a one-byte `SafeRead` probe. A bad FText pointer could still crash the process.
- Names resolve in a background thread over up to 10 minutes. Until then, the function-name checks do nothing.
