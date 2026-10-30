# SkyGfxDE → Android arm64 (libUE4.so via AML): feasibility map

Sources read: `/tmp/gtamod/skygfxde/src/*.cpp`, `skygfx.h`, `SkyGfxDE.ini`, `README.md`; `/tmp/gtamod/nm_dem.txt` and `nm_mangled.txt` (the same line number is the same symbol in both files); `/tmp/gtamod/repo/docs/arm64_map.md`, `mods_menu_design.md`, `mods/propfix/propfix.cpp` (AML API usage).

## 0. Trust and method

- **Observed:** the symbol names and ELF vaddrs below are copied from `nm_*.txt`. The PC mechanisms were read from source. Function sizes were inferred by subtracting neighbouring `nm` addresses.
- **Not done:**
  - No disassembly. `rizin` is missing and Ghidra/Hopper are unconfigured. `objdump` is macOS llvm-objdump, which rejects `--disassemble=`.
  - `grep` skips the binary `.so`, so no strings were checked: cvar names, UTF-16 strings and initial values of data symbols.
  - Every struct offset in a GTA class or a UE class is therefore `verify`. Use the reflection route in §1.
- **Offset-drift evidence:** `arm64_map.md` shows PC↔arm64 drift. `CPhysical` flags move 0x78→0x74, `UStaticMeshComponent::StaticMesh` moves 0x480→0x478, and the `FNamePool` `Blocks` offset moves 0x10→0x40. Treat no PC offset as equal until it is read on the device.
- **Key discovery:** this Android lib is not stripped of the GTA game layer. `CTimeCycle`, `CColourSet`, `CWeather`, `CClock`, `CPostEffects`, `CClouds`, `CCoronas`, `TheCamera`, `CPad`, `CGame` and `CCutsceneMgr` exist, with both functions and data symbols. Almost every PC `FindUnique` signature becomes a `dlsym`.
- **Thread assumption `[INFERENCE]`:** the PC mod treats `CTimeCycle::Update` as not on UE's game thread (`core.cpp:1121`, comment at `core.cpp:745-751`). Do UObject calls only inside actor ticks (`AGTATimeOfDay::Tick`, `AGTAWorldSettings::Tick`), as the PC does. Capture the game thread id there. `GGameThreadId` is also exported (`B 0xaf2bcdc`).

## 1. Android platform primitives (replace the PC infrastructure)

| PC infrastructure | Where | Android replacement |
|---|---|---|
| MinHook + `FindUnique` byte signatures + `RipAt` | `core.cpp:284-314`, `core.cpp:1140-1243` | `aml->GetSym(hUE4, "<mangled>")` + `aml->Hook(target, detour, &orig)`. Data symbols are exported, so `aml->GetSym` returns the data address. Pattern: `propfix.cpp:2079-2131`. |
| `FindIntCVar` / `FindRefCVar` code-scan for cvar storage | `core.cpp:316-343`, `streetlights.cpp:25-41`, `modern.cpp:~44-62` | `IConsoleManager::Singleton` is exported (`_ZN15IConsoleManager9SingletonE`, B 0xaeec6c8). `_ZNK15FConsoleManager19FindConsoleVariableEPKDsb` is at 0x6271164; its argument is a UTF-16 string. Then call `IConsoleVariable::Set/GetInt` through the vtable (slot `verify`). Alternative: `_ZN15FConsoleManager23ProcessUserConsoleInputEPKDsR13FOutputDeviceP6UWorld` (0x6273e88) with `"r.X 1"`. Use `ECVF_SetByConsole` priority so scalability does not undo it. |
| `FindName` pool walk, `NameOf`, `ClassOf` | `uobject.cpp:1-78` | `_ZN5FNameC1EPKc9EFindName` (0x64f4c90, `FNAME_Find`=0) gives the comparison index. `UObject` name at +0x18 and class at +0x10 are confirmed on arm64 by `arm64_map.md` O28/O29. `GUObjectArray` (B 0xaf558e8): chunk table at +0x10, `NumElements` at +0x24, item size 0x18 (`arm64_map.md` O38/O39). So PC `g_objects` = `&GUObjectArray + 0x10`. |
| `OnGameThread()` via `EnumWindows` | `uobject.cpp:49-64` | Drop. Anchor on a game-thread hook (actor `Tick`) and store `gettid()` there, or compare with `GGameThreadId`. |
| Hard-coded UE/GTA struct offsets (`PP::`, `TOD::`, `FOG::`, `SCS::`) | `core.cpp:347-396` | **Resolve by reflection at startup:** `UStruct::FindPropertyByName(FName)` is exported (`_ZNK7UStruct18FindPropertyByNameE5FName`, 0x670b530). The property's `Offset_Internal` gives the offset. Applies to `AGTATimeOfDay`, `AGTAHeightFog`, `FPostProcessSettings`, `FSkySettings` and `ULightComponentBase`. Bitfields (`bOverride_*`, `CastShadows`) need `FBoolProperty` byte/mask. `FPostProcessSettings::StaticStruct()` is at 0x9be30cc. |
| ImGui menu, hotkeys, `.ini` via `GetPrivateProfileA` | `overlay.cpp`, `core.cpp:33-229` | Mods menu (`mods_menu_design.md`): ini keys + `extern "C" void GTAML_OnConfigChanged()`. Ini API as in `propfix.cpp:91-134` (`aml->GetConfigPath()`; AML `GetPrivateProfile*` shims exist there). |
| `Present`/`OMSetRenderTargets` hooks, D3D11/12 | `overlay.cpp:380-614`, `postfx.cpp` | None. Replaced by UE post-process settings. See §3. |

**ModMenu key constraints** (`mods_menu_design.md`):
- Bool keys toggle.
- Choice keys (`; N = text` comments) cycle.
- Int keys step 1.
- Float keys step 0.1 and keep their decimals. Values that start non-negative stop at 0.
- String keys are read-only.
- Distances and counts therefore need coarse units (see §6).

## 2. Feature-by-feature

Effort: **S** ≤ ½ day, **M** 1–2 days, **L** multi-day or needs content/reverse work.

### 2.1 PS2 colour filter / grade (`[ColourFilter]`, `[Grade]`)

1. **PC mechanism**
   - Hook `AGTAPostProcessVolume::UpdateColorOptions` by signature (`core.cpp:1146`, hooked at `core.cpp:1199`). Handler: `core.cpp:695-715`.
   - After DE rewrites a volume's options, the mod snapshots DE's `ColorGain/Saturation/Contrast` and override bits (`core.cpp:595-602`, `669-692`). `WriteVolume` (`core.cpp:639-666`) then writes `ColorGain = DE gain × PS2 gain`, `ColorSaturation × Saturation`, `ColorContrast × Contrast`.
   - Offsets: `APostProcessVolume::Settings` +0x260; override byte 0 bits 2/3/5; `ColorSaturation` +0x30, `ColorContrast` +0x40, `ColorGain` +0x60 inside the settings (`core.cpp:376-387`).
   - `IsInterior` +0x864 on `AGTAPostProcessVolume`.
   - PS2 gain: `ComputePs2Gain` (`core.cpp:579-592`) reads `CColourSet` `PostFx1` +0x78 and `PostFx2` +0x88 (float r,g,b,a, 0..255). The gain is `2·p1 + 2·p2·min(1, 2·a2)`, converted through UE 4.26 `FilmToneMap` (`FilmDisplay`/`LinearGainFor`, `core.cpp:541-577`). The film curve is read from the exterior volume at `Settings+0x184`.
   - Per frame: `PerFrame` `core.cpp:1017-1109` rewrites all tracked volumes.
2. **Android equivalent**
   - Hook `_ZN19AGTAPostProcessVolume17UpdateColorOptionsEv` (0x4be727c; exported, `T`). The same wrapper design works.
   - Other related exports:
     - `_ZN19AGTAPostProcessVolume11SetAutoExposureEf` (0x4be7170)
     - `_ZN19AGTAPostProcessVolume7TickEf` (0x4be70c4)
     - `AGTAPostProcessVolume::ActivePostProcess` (B 0xa9ec490)
     - `UGTASingleton::SetPostProcessSceneColorTint(APostProcessVolume*, FLinearColor const&)` (0x4b28d3c)
   - `CColourSet` offsets: `CTimeCycle::m_CurrentColours` is exported (`_ZN10CTimeCycle16m_CurrentColoursE`, B 0xaadfcf8). It is 0xB8 bytes (next symbol `m_bExtraColourOn` 0xaadfdb0), against the PC's 0xAC+0x16C. The `FSkyColorSet` block at +0xAC does **not** exist here. `PostFx1/2` at +0x78/+0x88, `FarClip` +0x50, `FogStart` +0x54, `ShadowStrength` +0x48 and `HighLightMinIntensity` +0x9C match the original `CColourSet` layout. All **verify**.
   - Alternative source with no struct layout: read the exported per-column tables (`m_fPostFx1Red/Green/Blue/Alpha`, `m_fPostFx2*`) with the hour and weather interpolation done by the mod. Or call exported `CTimeCycle::CalcColoursForPoint(CVector const&, CColourSet*)` (0x54a571c) into a private `CColourSet`.
   - `FPostProcessSettings` offsets: `verify`. Resolve by reflection (`ColorGain`, `ColorSaturation`, `ColorContrast`, `bOverride_*`, `BloomIntensity`, `AutoExposureBias`).
   - Film curve (`FilmSlope…FilmWhiteClip`) must be re-read from the Android exterior volume.
3. **D3D/Win32 dependence:** none. It is a UE post-process volume write. **Risk:** if Android runs the mobile renderer, `FMobileTonemapPS`/`FMobileFilmTonemapParameters` exist. The mobile tonemapper honours grading but may not use the same film curve, so the `LinearGainFor` calibration may be off. Make the strength sliders do the calibration.
4. **Effort M, risk medium.** The hook is S. The calibration on the mobile tonemap is the M.

### 2.2 PS2 timecyc load (`[Timecyc] File`)

1. **PC mechanism**
   - Parse an 8-hour or 24-hour `timecyc.dat` (`core.cpp:453-505`). Write each column into DE's per-hour per-weather arrays `arr[h*23 + w]` (`core.cpp:489-503`), with kinds `K_U8` / `K_X10` / `K_I16` (`core.cpp:409-424`).
   - The array RVAs are recovered from `CColourSet::CColourSet` machine code: the n-th `movzx/movsx` load (`ResolveTimecycTable` `core.cpp:428-450`, 49 loads).
   - Applied after `CTimeCycle::Initialise` (hook `core.cpp:1129-1133`, signature `core.cpp:1147`).
2. **Android equivalent** (fully symbol-driven, no code scan):
   - Hook `_ZN10CTimeCycle10InitialiseEb` (0x545ab58, `T`) and write after the original returns.
   - **Every column is an exported data array.** Strides prove they are 8×23 bytes (0xB8) per colour channel. Examples: `m_nSkyTopRed` 0xaaddb81 → `m_nSkyTopGreen` 0xaaddc39 is +0xB8. `m_fFarClip` 0xaade872 → `m_fFogStart` 0xaade9e2 is +0x170, so int16. `m_fPostFx1Red` 0xaadf33a → `Green` 0xaadf3f2 is +0xB8, so u8.
   - Column → symbol map (`_ZN10CTimeCycle<len><name>E`):

     | Column group | Symbols |
     |---|---|
     | ambient r,g,b | `m_nAmbientRed/Green/Blue` |
     | ambient obj | `m_nAmbientRed_Obj/Green_Obj/Blue_Obj` |
     | directional | skipped, as on PC |
     | sky top | `m_nSkyTopRed/Green/Blue` |
     | sky bottom | `m_nSkyBottomRed/Green/Blue` |
     | sun core | `m_nSunCoreRed/Green/Blue` |
     | sun corona | `m_nSunCoronaRed/Green/Blue` |
     | sun size, sprite size, sprite brightness | `m_fSunSize`, `m_fSpriteSize`, `m_fSpriteBrightness` (u8 ×10) |
     | shadow strengths | `m_nShadowStrength`, `m_nLightShadowStrength`, `m_nPoleShadowStrength` |
     | far clip, fog start | `m_fFarClip`, `m_fFogStart` (i16) |
     | lights on ground | `m_fLightsOnGroundBrightness` (u8 ×10) |
     | low clouds | `m_nLowCloudsRed/Green/Blue` |
     | fluffy clouds | `m_nFluffyCloudsBottomRed/Green/Blue` |
     | water | `m_fWaterRed/Green/Blue/Alpha` |
     | postfx 1 | `m_fPostFx1Alpha/Red/Green/Blue` (file order a,r,g,b) |
     | postfx 2 | `m_fPostFx2Alpha/Red/Green/Blue` |
     | cloud alpha | `m_fCloudAlpha` |
     | intensity limit | `m_nHighLightMinIntensity` |
     | water fog alpha | `m_nWaterFogAlpha` |
     | directional mult | `m_nDirectionalMult`, skipped |

   - Encoding to reuse unchanged: `K_X10` = value×10 as u8; `K_I16`. **Verify element types** against the strides: `arm64_map.md` shows no typing, so check them once on the device.
   - Alternative to a post-Initialise hook: write before the first `CTimeCycle::Update` (from `GTAML` init). If DE reloads the tables on game restart, `CTimeCycle::InitForRestart` (0x54a7fdc, `T`) must also be hooked.
3. **D3D/Win32:** none (file open via `fopen` of the AML data path).
4. **Effort S, risk low.** The `timecyc_ps2.dat` is a data file that ships next to the `.so` (read via the AML config path, or an APK-extracted copy).

### 2.3 Timecyc colours (`[Colours]`: sky, fog, ambient, sun, clouds, brightness, night exposure)

1. **PC mechanism**
   - Hook the `AGTATimeOfDay` colour update (signature `core.cpp:1153`, hook `core.cpp:1201`; handler `Hooked_TodColours` `core.cpp:976-1003`). `ApplyColours` (`core.cpp:916-967`) moves `LiveColors` fields (`TOD` +0x2B8; `FSkyColorSet` fields `Skylight` 0x00, `SkyLower` 0x10, `SkyUpper` 0x20, `SkyReflection` 0x30, `VolumetricCloud` 0x60, `Fog` 0x118, `Sun` 0x158) towards the timecyc hue/brightness. The timecyc colours come from `m_CurrentColours`: `SkyTop` +0x24, `SkyBottom` +0x2A, `SunCore` +0x30, `LowClouds` +0x5C, `Ambient` +0x00 (`core.cpp:347-358`).
   - Brightness calibration: `CalibrateWeather` (`core.cpp:888-914`) calls DE's sky-curve evaluator (code `RipAt(ctor + 0x41D)`, `core.cpp:1193`) and a weather→curve-set table (`core.cpp:1192`) to scale to DE's noon brightness per weather.
   - Sun day balance: `DayFactor` `core.cpp:873-879`.
   - Night exposure: `WriteVolume` `core.cpp:651-657` (`AutoExposureBias` +0x314 on the exterior volume).
2. **Android equivalent**
   - `FSkySettings` exists (UStruct, `StaticStruct<FSkySettings>` 0x4da7668). Its properties mirror the PC `FSkyColorSet` fields: `SkylightColor`, `SkyLowerColor`, `SkyUpperColor`, `SkyReflectionColor`, `FogColor`, `SunColor`, `MoonColor`, `StarsColor`, `VolumetricCloudColor`, `CloudParams`, `GTAFogParam_*`. Confirmed by the `Z_Construct_UScriptStruct_FSkySettings_Statics::NewProp_*` list.
   - `AGTATimeOfDay` has no PC-style `CTimeCycle`→TOD copy. Methods: `Tick(float)` (`_ZN13AGTATimeOfDay4TickEf`, 0x4c3f230), `Update(float)` (0x4c40380, ~0x110 bytes), `ApplySettings(FSkySettings&, float, float)` (`_ZN13AGTATimeOfDay13ApplySettingsER12FSkySettingsff`, 0x4c40490), `ApplyValues()` (`_ZN13AGTATimeOfDay11ApplyValuesEv`, 0x4c41e14, ~0xF9C bytes), `UpdateSkyLight()` (0x4c45bb0), `UpdateClouds(float)`, `UpdateCloudsParameters2()`, `UpdateExternalActors(float)`, `GetBaseSettings/GetRainySettings/GetCloudySettings/GetExtraSunnySettings`.
   - Best hook point `[INFERENCE]`: post-hook `ApplyValues` (the PC hook sits after the "LiveColors = TargetColors, then overrides" block). Confirm by dumping which method writes the live properties.
   - The `AGTATimeOfDay` property offsets (live `SkyUpperColor` etc., `SunBaseIntensity`, `SkyLightIntensity`) come from reflection: the `FindPropertyByName` route in §1. The PC values (+0x2B8/+0x458/+0x66C/+0x37BC/+0x4770/+0x688) are all `verify` and likely different.
   - Weather blend for calibration: `CWeather::OldWeatherType` (`_ZN8CWeather14OldWeatherTypeE`, B 0xad86a22), `NewWeatherType` (0xad86a24), `InterpolationValue` (0xad86a14) are exported. The game clock is `CClock::ms_nGameClockHours/Minutes` (0xaac9414/0xaac9415). The PC reads hours through an engine-singleton vfunc +0x378 (`core.cpp:996`); use the exported `CClock` data instead.
   - **The sky-curve evaluator and the weather→curve table** (`core.cpp:1192-1194`) have no named export. Options: drop the K-calibration and use a fixed `kDefaultK` per weather (`core.cpp:884`), or reconstruct each weather's noon colour via `AGTATimeOfDay::Get*Settings()`.
   - `CTimeCycle::m_fFarClip`/`m_fFogStart` stay readable as exported arrays/`m_CurrentColours`.
3. **D3D/Win32:** none. **Risk:** the Android sky properties may be re-written by Blueprint code later in the frame, so pick the hook point carefully.
4. **Effort M–L, risk medium-high.** `AGTATimeOfDay` is a Blueprint-heavy actor whose live data flow differs from the PC's.

### 2.4 Classic sky / fog (`[Atmosphere]`), `AGTATimeOfDay::Tick`, colour update

1. **PC mechanism** (`ClassicScope`, `core.cpp:733-770`)
   - Classic Atmosphere is one global flag byte (`g_classicFlag`, found as `cmp cs:UseLightingOverrides, dl` at `SetClassicAtmosphere+0x1E`, `core.cpp:1172`). The mod sets it only while `AGTATimeOfDay::Tick`, the fog actor's `UpdateColors`, the weather particles, the volumetric clouds, the moving fog and the rainbow run. Lighting stays modern.
   - Hooks: `AGTATimeOfDay::Tick` (`core.cpp:745-751`, signature `core.cpp:1154`), `AGTAHeightFog::UpdateColors` (`core.cpp:779-868`, signature `core.cpp:1147`), weather particles (`core.cpp:1155`, `761`), `CClouds::VolumetricCloudsRender` port (`core.cpp:1156`, `762`), moving fog and rainbow (`core.cpp:1223-1228`).
   - Colour update runs twice (Classic, then modern) and only Classic's sky/fog fields are kept (`kClassicFields` `core.cpp:969-974`, `Hooked_TodColours` `core.cpp:976-1003`).
   - GTA fog (`GtaFog`, `FogDistance`, `FogOpacity`): `bUseGTAValues` on the fog actor (+0x2A0), the cvar-like byte `gta.ShowVolumeFogInClassic` (`core.cpp:1184`), and the VGD-override CDO `StartDistance` at `+0x220` of `UClass::ClassDefaultObject` (+0x118) reached through `TOD+0x4770` (`core.cpp:1062-1075`).
   - Modern fog tuning: `Haze`, `GroundHaze` write the height-fog component's `FogDensity` +0x1F8, second density +0x200, inscatter colour +0x20C (`FOG::` `core.cpp:388-395`); `MarkRenderStateDirty` (signature `core.cpp:1152`).
   - Cloud density patch: the 10/255 → 1/255 immediate in the volumetric-cloud function body (`core.cpp:1206-1221`).
2. **Android equivalent**
   - The setting exists in the game: `FGTASettings_Graphics::ClassicAtmosphere`, `UGameterSettings::OnClassicAtmosphereSet()` (`_ZN15UGameterSettings23OnClassicAtmosphereSetEv`, 0x4b564e0; verify mangled length).
   - **The flag** `AGTATimeOfDay::s_UseLightingOverrides` is exported (`_ZN13AGTATimeOfDay22s_UseLightingOverridesE`, D 0xa93af9e). There is also `s_UseNewNewLightingOverrides` (B 0xa9ed108) and `s_GetNonRegionBasedWeather` (D 0xa93af9c). **Which of the two is the PC `g_classicFlag` is unknown.** Probe: toggle the in-game option and log both bytes. The PC flag's meaning could be inverted ("use overrides").
   - Hook anchors, all exported:

     | PC hook | Android symbol |
     |---|---|
     | `AGTATimeOfDay::Tick` | `_ZN13AGTATimeOfDay4TickEf` (0x4c3f230) |
     | colour update | `_ZN13AGTATimeOfDay11ApplyValuesEv` (0x4c41e14) `[INFERENCE]`; alternatives `ApplySettings` (0x4c40490), `UpdateSkyLight` (0x4c45bb0), `ToggleOverrides` (0x4c3c1ac), `Update` (0x4c40380) |
     | `AGTAHeightFog::UpdateColors` | `_ZN14AGTAHeightFog12UpdateColorsEf` (0x4bbeb3c); also `UseGTAFog() const` (`_ZNK14AGTAHeightFog9UseGTAFogEv`, 0x4bbf724 = PC `ShouldUseGTAFog`), `OptionsChanged` (0x4bbf4a0) |
     | volumetric-cloud port | `_ZN7CClouds20VolumetricCloudsRenderEv` (0x560b934) |
     | moving fog | `_ZN7CClouds15MovingFogRenderEv` (0x560ca10) |
     | rainbow | not found by name. `CWeather::Rainbow` data is exported (B 0xad7fff0); the render function is probably folded into `CClouds::Render` (0x560acbc) or `RenderSkyPolys` (0x560ade8). `needs sig scan`/skip |
     | sand/wind spray | `_ZN8CWeather21AddSandStormParticlesEv` (0x5648fa0), `AddRain` (0x5648910) |

   - `gta.ShowVolumeFogInClassic`: no export. It is a cvar-like static: `needs sig scan`, or drop GTA-fog mode and keep Classic and Modern.
   - The cloud 1/255 patch: needs an arm64 immediate search in `VolumetricCloudsRender`. **Skip it first.**
   - `AGTAVolumetricCloud` methods are exported (`SetSkyParams`, `SetQualityLevel`, `UpdateCloudiness`). Cloud density can be tuned there without patching.
3. **D3D/Win32:** none. **Risk:** Classic mode differs on the mobile renderer, which has no volumetric-cloud pass. Check `GMaxRHIFeatureLevel` (D 0xa96ddd8) before spending time on cloud work.
4. **Effort L, risk high.** The scoped-flag trick is easy (S) once the flag is identified. The fog and cloud tuning is M–L. Expect to drop GTA-fog mode and the cloud density patch at first.

### 2.5 `r.VolumetricCloud` cvar

1. **PC mechanism:** `FindIntCVar(L"r.VolumetricCloud")` (`core.cpp:1186`; scan `core.cpp:321-343`). In `PerFrame` the mod writes `v[0]=v[1]=0` (game and render copies of the `TConsoleVariableData`) while the Classic sky is on, and restores DE's value otherwise (`core.cpp:1017-1109`, the cvar block is near the top of `PerFrame`).
2. **Android equivalent:** no exported `CVarVolumetricCloud`. Use `IConsoleManager::Singleton->FindConsoleVariable(u"r.VolumetricCloud")`, then `Set(u"0", ECVF_SetByConsole)` and `GetInt` for the save/restore. Or `ProcessUserConsoleInput`.
3. **D3D:** none. **Conditional:** the cvar only matters at SM5.
4. **Effort S, risk low.**

### 2.6 Coronas / SALodLights (`coronas.cpp`)

1. **PC mechanism**
   - Hook `CFileLoader::LoadObjectInstance(CFileObjectInstance*, name, bool)` (signature `coronas.cpp:260`, hook `270`, handler `~106-112`). `Catch` (`~87-104`) turns every IPL instance of a listed model into lamps.
   - Structs: `CBaseModelInfo` `Key` +0x08, `DrawDistance` +0x38; `CFileObjectInstance` quaternion/pos at +0x00, `Model` +0x1C, `Area` +0x20 (`coronas.cpp:18-19`). Heading from the quaternion.
   - `CKeyGen::GetUppercaseKey` reimplemented as CRC (`coronas.cpp:36-46`). `CModelInfo::ms_modelInfoPtrs` via the `lea r15` at +0x15 (`coronas.cpp:262`).
   - Render via UE: `AGTAWorldSettings::GetCoronaComponent(bDynamic)` (`UGTACoronaComponent`) fed through `ProcessEvent` `AddCorona`/`UpdateCorona`/`RemoveCorona` (`coronas.cpp:110-112`, `Resolve` `~115-150`, `Show`/`Hide`). The settings object is found by a GObjects walk (`WS::StaticCoronas` +0x3B0). Parameter structs are laid out by hand (`coronas.cpp` `Show`).
   - Game state: hours/minutes/ms/`currArea` from `CClock`/`CTimer`/`CGame`. `g_fx.cam` is the camera position, from `TheCamera.m_matrix` (`tools.cpp:183-186`, `288`).
   - Data file `SALodLights.dat` (`LoadDat` `~48-85`), p2dfx blink/alpha math (`LampAlpha`, `NightAlpha`).
2. **Android equivalent**
   - Hook `_ZN11CFileLoader18LoadObjectInstanceEPN10SanAndreas19CFileObjectInstanceEPKcb` (0x5450ff8, exported). There is another overload `(char const*, int&)` at 0x5449b80. `CFileObjectInstance` layout `verify`.
   - `CModelInfo::ms_modelInfoPtrs`: `_ZN10CModelInfo16ms_modelInfoPtrsE` (B 0xad272a8). It is the array itself, as `arm64_map.md` S11 notes. Not a pointer-to-pointer.
   - `CKeyGen::GetUppercaseKey(char const*)` is exported (0x54669c8). Call it instead of the CRC copy; `GetKey` at 0x546665c.
   - Corona component is native, no `ProcessEvent` needed: `UGTACoronaComponent::AddCorona(FVector,float,float,FColor)` (0x4b85f80), `UpdateCorona(int,FVector const&,float,FColor,float)` (0x4b868bc), `RemoveCorona(int)` (0x4b86ab4), `ConditionalUpdate` (0x4b86780). Argument order of the two floats in `AddCorona`: `verify` (PC `Show` passes intensity then scale in the parameter struct). Safe fallback: keep `UObject::FindFunction`/`ProcessEvent` as `propfix` does.
   - `AGTAWorldSettings::GetCoronaComponent(bool,bool)` (`_ZN17AGTAWorldSettings18GetCoronaComponentEbb`, 0x4c8afa0). Get the settings object with `UGTASingleton::GetGTAWorldSettings(UObject*)` (0x4c22e08) or `UWorld::GetWorldSettings(bool,bool)` (0x9696a1c), or capture `this` in a hook of `AGTAWorldSettings::BeginPlay` (0x4c8aea0). That replaces the GObjects walk and the `+0x3B0` offset.
   - Game state: `CClock::ms_nGameClockHours/Minutes`, `CTimer::m_snTimeInMilliseconds` (B 0xaae2404), `CGame::currArea` (B 0xaace2d8). All exported.
   - Camera position: `TheCamera` (B 0xaac6700) + matrix at +0x18 `verify`; or `FindPlayerCoors(int)` (0x548a8a8).
   - The game already has native coronas: `CCoronas::RegisterCorona` (0x560e69c / 0x560e650), `CCoronas::Update` (0x560d6d8), `CCoronas::Render` is an 8-byte stub (0x560d814). Android may already draw SA coronas through `AGTALightBase::SetFarCorona`. `GetCoronaDetails` (0x4bbc550) and `AGTALightBase::SetFarCorona(float,float)` (0x4bbcfa4) are exported.
3. **D3D/Win32:** none. **Risk:** coordinate transform (Y flipped, ×100) is the same engine contract, but the mobile LOD distance and mobile far-clip may already hide distant lamps. The data file must ship with the mod.
4. **Effort M, risk low-medium.**

### 2.7 Darker shadows (`[Shadows] Darkness`)

1. **PC mechanism:** `PerFrame` (`core.cpp:1017-1109`, shadow block near the end). `g_indirect = clamp(1 − Darkness × shadowStrength/255, 0.1, 1)`, where `shadowStrength` is `CColourSet::ShadowStrength` (+0x48, i16). It scales `AGTATimeOfDay::SkyLightIntensity` (+0x66C), saving DE's value and restoring it. DE sets the sky-light intensity at `0x140BBF6D0` as `SkyLightIntensity × SkylightColor.a`.
2. **Android equivalent**
   - Property `SkyLightIntensity` via reflection (a `NewProp_SkyLightIntensity` exists on `AGTATimeOfDay`, `Z_Construct_UClass_AGTATimeOfDay_Statics::NewProp_SkyLightIntensity`). Sky-light update is `AGTATimeOfDay::UpdateSkyLight()` (0x4c45bb0).
   - Shadow strength: `CTimeCycle::m_nShadowStrength` table, or `m_CurrentColours+0x48` `verify`.
   - Approach is unchanged: scale before `UpdateSkyLight` runs, then restore.
3. **D3D/Win32:** none. **Risk:** the mobile renderer's sky-light handling may ignore this value; on-device test.
4. **Effort S, risk low-medium.**

### 2.8 Radiosity (`[Radiosity]`)

1. **PC mechanism:** a D3D11 pass (`postfx.cpp:230-247`). The frame is halved to PS2 scale; the highlights above `m_nHighLightMinIntensity × 128/255` (`CColourSet +0x9C`) are doubled and added back at alpha `Intensity/255` (35) with `SRCALPHA/ONE`. DE bloom is scaled by `DEBloom` (default 0) on every volume, via `BloomIntensity` +0x21C and bloom override bit byte 6 bit 2 (`WriteVolume` `core.cpp:658-666`).
2. **Android equivalent (alternative, not a port):** use UE bloom on the same volumes. `FPostProcessSettings` has `BloomIntensity`, `BloomThreshold`, `BloomSizeScale`, `Bloom1Size…Bloom6Size`, `Bloom1Tint…Bloom6Tint`, `BloomMethod`, `BloomDirtMask`, `bMobileHQGaussian` (all confirmed in the `_FPostProcessSettings_Statics::NewProp_*` list). Map:
   - `BloomThreshold` ≈ `limit/255` (timecyc `m_nHighLightMinIntensity`, which is a per-hour/weather table; the PS2 file makes it 90 at midnight, 220 at midday, so night glows more).
   - `BloomIntensity` ≈ 35/255 × a constant to tune.
   - A larger `Bloom1/2Size` for the soft glow.
   - Mobile renderer has `FMobileBloomSetupPS/DownPS/UpPS` and `FMobileSunMerge`, so bloom threshold/intensity/size exist there.
   - Native game path: `CPostEffects::Radiosity(int,int,int,int)` (0x5624c0c, ~0x35c bytes), `m_bRadiosity` (B 0xad7c2d9), `m_RadiosityIntensity` (D 0xa9656d8), `m_RadiosityFilterPasses` (0xa9656cc), `m_RadiosityIntensityLimit` (0xa9656d4), `m_bRadiosityBypassTimeCycleIntensityLimit` (0xad7c2e5). Cheap probe: set `m_bRadiosity=1` and see whether the RW-raster code draws anything. `CPostEffects::SetupBackBufferVertex` (8 bytes), `RasterCreatePostEffects` (8 bytes) and `Grain` (4 bytes) look like stubs, so `[INFERENCE]` the native path draws nothing.
3. **D3D dependence:** yes (full-screen copy/downsample/blend). **Alternative:** UE bloom (above). Drop the exact PS2 shape.
4. **Effort M (tuning), risk medium.**

### 2.9 SpeedFX (`[SpeedFX]`)

1. **PC mechanism**
   - Trigger logic in the game thread: `SpeedFxRowUpdate` (`tools.cpp:267-~320`). Player ped from `CWorld::Players` (stride 0x1C0) and `PlayerInFocus`; vehicle at ped +0x7C8 when ped flags +0x634 & 0x100; `m_nVehicleSubType` +0x894 (3..6 excluded); move speed at +0x7C (`ENT::MoveSpeed`); nitro by `handlingFlags +0x5C0 & 0x80000`, `m_fTireTemperature +0xC0C`, `m_GasPedal +0x710`; camera look direction at `TheCamera + 0x1C4 + 0x1B8 * active` and active cam at +0x5B (`tools.cpp:270-310`).
   - The original's table `kSpeedFx[7]` and alpha 36 (`skygfx.h:79-83`). The pass itself is D3D11 (`postfx.cpp:202-228`): zoomed copies at alpha 36.
2. **Android equivalent**
   - Inputs: `CWorld::Players` (B 0xab22da0), `CWorld::PlayerInFocus` (B 0xab23120), `FindPlayerVehicle(int,bool)` (0x547fe78), `FindPlayerEntity(int)` (0x548abf0), `CCutsceneMgr::ms_running` (B 0xaa1f330), `TheCamera` (B 0xaac6700).
   - Offsets: `CPhysical::m_vecMoveSpeed` is **0x78** (not 0x7C): `arm64_map.md` O7, resolved. `CVehicle` subtype, handling flags, tire temperature, gas pedal: all `verify`; PC +0x894/+0x5C0/+0xC0C/+0x710 may shift by about -4 like `CPhysical`. The `CCamera` cam-array offsets: `verify`.
   - Native game path exists: `CPostEffects::SpeedFX(float)` (`_ZN12CPostEffects7SpeedFXEf`, 0x5624f68, ~0x39c bytes), `m_bSpeedFX` (D 0xa9656ac), `m_SpeedFXAlpha` (D 0xa9656b0), `m_bSpeedFXTestMode` (B 0xad7c2ba), `m_fSpeedFXManualSpeedCurrentFrame` (B 0xad7c2bc), `m_bSpeedFXUserFlag` (D 0xa9656b4). Probe `m_bSpeedFXTestMode=1`.
3. **D3D dependence:** yes (frame copy and zoom). **Alternative:** there is no UE zoom/radial blur in `FPostProcessSettings`. Closest: raise `MotionBlurAmount`/`MotionBlurMax`/`MotionBlurTargetFPS` with speed (the game also has `CCamera::SetMotionBlur`/`SetMotionBlurAlpha`/`RenderMotionBlur`, and `UGameterSettings::OnMotionBlurSet`); or `SceneFringeIntensity`; or a small FOV kick through the camera. All approximate. Or **drop**.
4. **Effort M for the trigger logic + approximation, risk medium.** Exact PS2 look: not portable.

### 2.10 Grain (`[Grain]`)

1. **PC mechanism:** game-thread state in `SpeedFxRowUpdate`: strength steps toward `128 × CWeather::Rain` and the mask is `strength/4` outside no-rain zones, above water, `currArea==0`, below 900 m (`tools.cpp:~296-312`). Render: D3D11 64×64 PS2-random texture, tiled, blended as `dst·(1+2a)` (`postfx.cpp:249-265`).
2. **Android equivalent:** inputs are exported: `CWeather::Rain` (B 0xada6a50), `CWeather::UnderWaterness` (B 0xada6a44), `CCullZones::CurrentFlags_Camera/Player` (B 0xad80080 / 0xad80220), `CGame::currArea`. The no-rain test can call `CCullZones::CamNoRain()` (0x56488e8) / `PlayerNoRain()` (0x56488fc) directly instead of reading flag bits. `CPostEffects::Grain(int,bool)` is a 4-byte stub (0x5625448).
3. **D3D dependence:** yes. **Alternative:** `FPostProcessSettings::GrainIntensity` (+`bOverride_GrainIntensity`) and `GrainJitter` (both confirmed in the property list), driven by the PC's ramp. No texture. **Effort S, risk low** (check the mobile tonemapper applies grain; `FMobileTonemapPS` has a grain permutation in stock 4.26 `[INFERENCE]`).

### 2.11 Water drops (`[WaterDrops]`)

1. **PC mechanism:** a 2000-drop screen-space simulation drawn in D3D11 (`postfx.cpp:267-442`). Sources: rain, three splash FX (`tools.cpp:322-381`, hooks at `tools.cpp:572-585`), boat/wake particles via `FxSystem_c::AddParticle` (`tools.cpp:586-600`), hydrant audio event (`tools.cpp:582-585`). State from `g_fx` (`skygfx.h:88-97`).
2. **Android equivalent:** the source hooks have no exact exports I searched for (`water_splash` FX wrappers, `FxSystem_c::AddParticle` Android name not looked up). **No UE setting exists for lens drops.** Options:
   - `APlayerCameraManager::AddCameraLensEffect(TSubclassOf<AEmitterCameraLensEffectBase>)` (exported, 0x926ed8c), but it needs a lens-effect class asset that the game must already ship. None found by name.
   - A UMG widget with a dynamic material: needs a cooked material asset we don't have.
   - A post-process material blendable: same.
3. **D3D dependence:** yes. **Recommendation: drop** (Phase 3 only if an asset turns up). **Effort L, risk high.**

### 2.12 Street-light pools and shadows (`streetlights.cpp`, `[StreetLights]`)

1. **PC mechanism**
   - `FindRefCVar` locates the storage of the int cvars `gta.streetlightdistance` and `gta.streetlight.shadowdistance` (`streetlights.cpp:25-48`). Each frame `StreetLightsFrame` (`~50-66`) sets them (cm; DE's are 4 cm → lamps culled and unshadowed). DE applies them to lamps as they stream in (lamp update 0x140C206C0, `ULightComponent +0x22C`/`+0x324`).
   - `OtherLightShadowsFrame` (`streetlights.cpp:~112-240`, constants `77-82`): scans GObjects (4096 slots/frame) for point/spot lights whose owner class is not a skip-listed vehicle/effect. Sets `CastShadows` (+0x214 bit 1) and `CastDynamicShadows` (bit 3, forced via `SetCastShadows(false/true)` through `ProcessEvent`). `AStreetLightMapActor::bNeverCastShadows` +0x300 bit 7 is cleared. Fixture meshes with bounds radius <600 stop casting shadows (`UPrimitiveComponent::SetCastShadow`; +0x213 bit 5, +0x118 radius). Bollard/globe brightness via `ULightComponent::SetIntensity` (+0x20C).
2. **Android equivalent**
   - The cvar variables are **exported globals**: `GStreetLightDistance` (D 0xa93b4e8), `GStreetlightShadowDistance` (B 0xa9f004c), and the `FAutoConsoleVariableRef` objects `CVarStreetlightDistance` (B 0xa9f0090), `CVarStreetlightShadowDistance` (B 0xa9f0050), `CVarStreetlightColorVariation` (B 0xa9f0060), `GStreetlightColorVariation` (D 0xa93b4f0). Write the globals directly after `dlsym`. **Verify int vs float and the units** by logging the value at startup (do not assume 4 cm). Or go through the console manager with the `gta.streetlightdistance` names (unverified strings).
   - Lamp actor: `AStreetLightMapActor` is present (`StaticClass` 0x4de33c4). Relevant natives: `UpdateAll(float,FVector const&,bool,bool)` (0x4cde9ac), `SetLights(bool,bool)` (0x4de3144), `SetLightVisibility(ULightComponent*,bool)` (0x4cde7f0), `SetLowQualityLights(bool)` (0x4cdd410), `SetAsLowQualityLightTemplate()` (0x4cdefcc), `BuildDecal()`/`CreateDecalComponent`/`StreetDecalComponent`, `OptionsChanged()` (0x4cdce28), `bForceUpdate` (B 0xa9f00b0). **Android has a low-quality-lights path with light-pool decals** (`BuildDecal`, `AddToComponent(UStreetDecalComponent*, FProcMeshSection*)`). The PC "4 cm, no light pools" situation may differ here: check which lamp mode is active.
   - Per-light setters are now **native exports**, so no `ProcessEvent` and no UFunction lookup: `_ZN19ULightComponentBase14SetCastShadowsEb` (0x8bbcdc4), `_ZN15ULightComponent12SetIntensityEf` (0x8bbed74), `_ZN19UPrimitiveComponent13SetCastShadowEb` (0x8bd9930). Also `UGTASingleton::SetShadowRenderDistance(ULightComponent*, float)` (0x4c22de0), `SetMaxDrawDistance(ULightComponent*, float, float)` (0x4c22df0), `SetLightingChannels(USceneComponent*,bool,bool,bool)` (0x4c22b84), `SetShadowResolutionScale` (0x4c22dd0). These may be the exact setters the lamp update uses.
   - `ULightComponentBase` bitfield (+0x214), `Intensity` (+0x20C), `Bounds` (+0x118), `AStreetLightMapActor::bNeverCastShadows` (+0x300): all `verify`. Reflection resolves them.
   - Object scan: Android `GUObjectArray` iterate as in §1; `UObject::Flags` at +0x08 as on PC (`arm64_map.md` O27). `IsA` by walking `SuperStruct` at +0x40 is confirmed (O30).
3. **D3D/Win32:** none. **Risk:** frame-time on a phone. The PC mod notes "each lamp shadow renders a map every frame". Make `ShadowDistance` conservative by default (e.g. 0 or ≤ 20 m) and gate `OtherLightShadows` off by default.
4. **Effort M (cvar writer S; the all-lights shadow scan M), risk medium-high on performance.**

### 2.13 LOD distance (`[World] LodDistance`)

1. **PC mechanism:** after `CCamera::Process`, multiply `TheCamera.m_fLODDistMultiplier` (+0x104) by `LodDistance` (`tools.cpp:382-395`). The offset was found by the `movss 70.0 / divss FOV / movss [rbx+104h]` store pattern (signature `tools.cpp:564`; `g_lodMult` `tools.cpp:565-569`). Cutscenes keep DE's value.
2. **Android equivalent**
   - Hook `_ZN7CCamera7ProcessEv` (0x5426668, exported). `TheCamera` is exported (B 0xaac6700); the PC `g_camMatrix - 0x18` base trick is unnecessary.
   - `m_fLODDistMultiplier` has no symbol. `needs sig scan`: inside `CCamera::Process` find the `70.0f` constant (0x428C0000, arm64 `movz w,#0x428c,lsl 16`; 70 is not an `fmov` immediate), the following `fdiv` and the two adjacent `str s,[xN,#imm]` stores. The immediates give the field offset. `[INFERENCE]` The 0x104 PC offset may be kept, but verify.
   - Other draw-distance knobs that exist in UE: `CVarStaticMeshLODDistanceScale` (B 0xb0230b0), `CVarFoliageLODDistanceScale`, `GLightMaxDrawDistanceScale` (D 0xa96ed3c), `GCalcLocalPlayerCachedLODDistanceFactor`. These affect UE meshes, not GTA's per-model draw distance. Also `AGTAWorldSettings::FixLODs/ResetLODs/OscillateLODs` are 4-byte stubs.
3. **D3D/Win32:** none. **Risk:** more draw distance costs frames on a phone. Cutscene guard via `CCutsceneMgr::ms_running`. **Effort S–M (sig scan), risk low-medium.**

### 2.14 Before-HUD drawing (`[SpeedFX] HudBind`)

1. **PC mechanism:** hook `ID3D11DeviceContext::OMSetRenderTargets` vtable slots 33/34 and count backbuffer binds, drawing the post effects before bind `HudBind` (=2) (`overlay.cpp:380-421`; D3D12 path `overlay.cpp:473-536`; `Present` hook `overlay.cpp:423-447`).
2. **Android equivalent:** none needed. UE's post-process (tonemap, bloom, grain) runs before Slate/UMG, which is where the HUD is drawn. Anything done with `FPostProcessSettings` is automatically before the HUD.
3. **D3D dependence:** yes (D3D11/12 vtables). **Drop.** **Effort 0, risk none.** Note that the HUD is a separate path: `CHud::Draw` (0x54f7458) and `CSprite2d` → `RwIm2DRenderPrimitive` (0x4ae8268).

### 2.15 Debug menu: weather / time / freecam / noclip (`tools.cpp`, `ToolsPanel`)

1. **PC mechanism**
   - Weather and clock via `CWeather::Update` hook (`tools.cpp:126-163`, signature `tools.cpp:498`). Writes `OldWeatherType`, `NewWeatherType`, `ForcedWeatherType`, `InterpolationValue` (`tools.cpp:142-156`). Time: `CClock::SetGameClock` (`tools.cpp:127-132`), `ms_nMillisecondsPerGameMinute`/`ms_nLastClockTick`/seconds for speed and freeze (`tools.cpp:133-139`). Globals found by `RipAt` into code (`tools.cpp:520-549`).
   - Freecam/noclip: after `CCamera::Process` (`AfterCamera` `tools.cpp:183-258`). Freecam overwrites `TheCamera` matrix rows (right, forward, up, pos; `tools.cpp:229-243`). Noclip clears `bUsesCollision` (+0x30 bit 0), zeroes move/turn speed (+0x7C/+0x88) and moves the matrix position (`tools.cpp:195-211`). Input is WASD/Space/C/Shift/Alt/wheel (`MoveInput` `tools.cpp:101-116`); the look direction comes from the game's own mouse camera.
   - The ImGui panel (`tools.cpp:407-483`): weather combo, blend slider, hour/minute, timecyc keyframe buttons, freeze, time speed, timelapse, LOD, freecam/noclip toggles and speed, "move player to camera".
2. **Android equivalent**
   - Weather/time are **fully exported**:
     - `CWeather::ForceWeatherNow(short)` (0x5648e1c), `ForceWeather(short)` (0x5648e0c), `ReleaseWeather()` (0x5648f88), `ForecastWeather(int,int)`, `SetWeatherToAppropriateTypeNow()` (0x5648e68).
     - Data: `OldWeatherType` (0xad86a22), `NewWeatherType` (0xad86a24), `ForcedWeatherType` (0xad86a12), `InterpolationValue` (0xad86a14), `WeatherRegion` (0xada6a58), `CWeather::Update()` (0x5647850).
     - Clock: `CClock::SetGameClock(unsigned char,unsigned char,unsigned char)` (0x542f0ac), `ms_nMillisecondsPerGameMinute` (0xaac92a4), `ms_nLastClockTick` (0xaac92a8), `ms_nGameClockSeconds` (0xaac92b2), `Update()` (0x542f378).
     - So no `RipAt` anchors are needed.
   - **No keyboard or mouse.** Controls: weather, time, freeze, speed become ini keys (`mods_menu_design.md`). `ForceWeather` is an int key (−1 = game) polled in `GTAML_OnConfigChanged` plus a per-frame check. A named-choice cycle is also possible (`; N = text`).
   - Noclip:
     - Player entity from `FindPlayerEntity(int)` (0x548abf0).
     - Movement: `CPad::GetPad(int)` (0x545b8b0) → stick via `CPad::GetLeftAnalogue(CVector2D*)` (0x546e240); look via `CPad::GetRightAnalogue(CVector2D*)` (0x546e36c). Hook `CPad::GetPedWalkLeftRight/UpDown` (0x546addc / 0x546af9c) and `GetSteeringLeftRight` (0x546d678) to return 0 while noclip/freecam is on, so the game stops using the stick.
     - `CEntity` matrix pointer at +0x18, flags +0x30 are confirmed (`arm64_map.md` O1/O3). `bUsesCollision`=bit 0 is **not** confirmed (O3 note). `CPhysical` move speed at +0x78, turn speed +0x84 (O7/O8).
   - Freecam:
     - Game camera: overwrite the `TheCamera` matrix after `CCamera::Process` (0x5426668), as on PC. Game culling uses it (`CCamera::CalculateFrustumPlanes`, 0x5429dc8).
     - The UE view may not come from `TheCamera`. Two extra hook options: `APlayerCameraManager::UpdateCamera(float)` (0x926ba98) / `DoUpdateCamera` (0x926bfc4) or `UCameraComponent::GetCameraView(float, FMinimalViewInfo&)` (0x8b08048); patch `Location`/`Rotation` in the `FMinimalViewInfo`. Needs `[INFERENCE]` testing which one the game uses.
     - Look direction: on touch, the game's touch-camera look continues. Keep that as on the PC mouse design.
   - "Move player to camera": write the player entity matrix pos (same as PC).
3. **D3D/Win32:** the ImGui panel, `GetAsyncKeyState`, WndProc/cursor hooks (`overlay.cpp:135-228`) are PC-only. **Replace** with ini keys. **Effort:** weather+time S; noclip M; freecam M–L (UE view path, culling, streaming warning); touch-aware pad suppression M. **Risk:** medium (must not leave the player stuck in noclip).

### 2.16 `uobject.cpp` helpers

1. **PC mechanism:** `g_namePool` found by signature (`uobject.cpp:71-78`), `NameEntry`/`NameIs`/`FindName` walk the pool block by block (`uobject.cpp:10-43`); `NameOf`/`ClassOf` (`45-46`); `OnGameThread` (`49-64`); `LiveAt` (`66-69`). `UO::` offsets (`skygfx.h:159`): `Flags` 0x08, `Class` 0x10, `Name` 0x18, `Outer` 0x20, `Index` 0x0C, `ProcessEventSlot` 0x43.
2. **Android equivalent**
   - `FName(const char*, FNAME_Find)` (`_ZN5FNameC1EPKc9EFindName`, 0x64f4c90) replaces `FindName`. `FName::GetEntry(FNameEntryId)` (0x64f4510) + `FNamePool::Blocks` at +0x40 replaces the pool walk when a reverse lookup is needed (`arm64_map.md` S13).
   - `ProcessEventSlot`: the arm64 slot is 0x220/8 = **0x44** (`arm64_map.md` O44: vtable offset 0x220; PC 0x43×8 = 0x218). Or hook/call `_ZN7UObject12ProcessEventEP9UFunctionPv` (0x67a758c) directly.
   - UObject +0x08/+0x0C/+0x10/+0x18/+0x20 confirmed (`arm64_map.md` O27-O29; Outer +0x20 is from the ctor store). `FUObjectItem` flags at +8 (bit 29 pending-kill, 28 unreachable): **unconfirmed** (O39b). Use `FUObjectArray::IsValid` (0x67c01ec) or `UObject::IsPendingKill`-style checks if exported.
3. **D3D/Win32:** `OnGameThread` uses `EnumWindows`: drop (see §1). **Effort S, risk low.** Most of the file is deleted.

### 2.17 Modern look: SSGI / GTAO / SSR / volumetric fog / lens flare / sun strength (`modern.cpp`, `[ModernLook]`)

Not on the user's list but part of the PC mod, so it is in the plan.

1. **PC mechanism:** `ApplyCVar` forces `r.SSGI.Enable`, `r.SSGI.Quality`, `r.AmbientOcclusion.Method`, `r.SSR.Quality`, `r.VolumetricFog` (`modern.cpp:~24-31`, `~65-85`), found via `FindIntCVar`/`FindRefCVar`. Volume fields (AO intensity/radius +0x40C/+0x414, SSR +0x4FC/+0x500/+0x504, lens flare +0x35C) are written in `ModernVolume` (`modern.cpp` tail; table `~88-98`). Sun strength scales `AGTATimeOfDay::SunBaseIntensity` (+0x37BC, `core.cpp` `PerFrame`).
2. **Android equivalent:** cvars via the console manager (§1). Android evidence of the renderer: the lib contains **both** desktop passes and `FMobileSceneRenderer` (`RenderDeferred` 0x7a1804c, `RenderForward` 0x7a19364, `RenderAmbientOcclusion` 0x7a1ac30/0x7a33144, `RenderPixelProjectedReflection` 0x7b033dc). The mobile path has **no SSGI, no SSR** (only pixel-projected reflection for planar), **no volumetric fog**, and no volumetric cloud. `CVarVolumetricFog` (B 0xb03f0d0) and `CVarSSRDisable` (B 0xb024c20) are exported, but the passes will not run at ES3.1. Check `GMaxRHIFeatureLevel` (D 0xa96ddd8) first: 3 = SM5 is the only level these apply to.
3. **D3D/Win32:** none. **Recommendation:** gate every modern toggle on `GMaxRHIFeatureLevel==SM5`; hide them otherwise. Sun strength and lens flare are the exceptions that can work on mobile (sun intensity is a plain light property; the mobile pipeline has no lens flare). **Effort S for cvar plumbing, value low on a phone.**

### 2.18 Not asked for but in the PC mod: weather particles, moving fog, rainbow, Modern fog haze

Covered inside §2.4. Rainbow has no named function export. Moving fog and `CClouds::` functions are exported.

## 3. Render alternatives, summary

| PC render feature | D3D? | Android alternative | Fidelity |
|---|---|---|---|
| PS2 colour filter + grade | no (post volume) | same volume write | high |
| Radiosity (glow) | yes | UE bloom (`BloomThreshold`, `BloomIntensity`, `Bloom1/2Size`, tints) | medium, retune |
| Grain | yes | `GrainIntensity`, `GrainJitter` (+ overrides) | medium |
| SpeedFX | yes | `MotionBlurAmount/Max`, optional `SceneFringeIntensity`, FOV kick | low; or drop |
| Water drops | yes | none without a cooked asset | drop |
| Before-HUD ordering | yes | automatic | n/a, drop |
| Fog / sky | no | actor properties (reflection offsets) | high |
| Street-light shadows | no | engine light flags; perf-limited | high |
| ImGui menu | yes | ini + ModMenu | n/a |

How to inject post-process settings without owning the volume:
- `APlayerCameraManager::AddCachedPPBlend(FPostProcessSettings&, float)` (0x9268a98).
- `UCameraComponent::AddExtraPostProcessBlend(FPostProcessSettings const&, float)` (0x8b0995c), `ClearExtraPostProcessBlends()`.
- These let the mod keep its own `FPostProcessSettings` (built from `FPostProcessSettings::FPostProcessSettings()`, 0x9369948) with only the grain/bloom/colour override bits set, rather than rewriting DE's volumes.
- Layout of the struct still comes from reflection.

## 4. Verification probes to run first on the device

Each answers a `verify` or `[INFERENCE]` above in one log line:
1. `GMaxRHIFeatureLevel` and `GMaxRHIShaderPlatform` (mobile vs SM5).
2. `AGTATimeOfDay::s_UseLightingOverrides` and `s_UseNewNewLightingOverrides`: before and after toggling Classic Atmosphere in game options.
3. `GStreetLightDistance`, `GStreetlightShadowDistance` (type and value at startup).
4. A hex dump of `CTimeCycle::m_CurrentColours` (0xB8 bytes) and the first two entries of `m_nSkyTopRed`: confirms the `CColourSet` offsets and the u8 element type.
5. `FindPropertyByName` offsets for `AGTATimeOfDay` (`SkyUpperColor`, `SkyLowerColor`, `SkylightColor`, `FogColor`, `SunColor`, `SkyLightIntensity`, `CloudyAmount`), `AGTAHeightFog` (`bUseGTAValues`, `HeightFogComponent`, `TimeOfDayActor`), `FPostProcessSettings` (`ColorGain`, `GrainIntensity`, `BloomIntensity`, …): compare to the PC values in `core.cpp:347-396`.
6. Which of `AGTATimeOfDay::ApplyValues`/`ApplySettings`/`Update` writes the live colour properties (call-count log + before/after property dump).
7. Set `CPostEffects::m_bSpeedFXTestMode=1`, `m_bRadiosity=1`: does the native game path draw anything?
8. `TheCamera` write test: does overwriting the matrix after `CCamera::Process` move the UE view?
9. ~~`CCamera::Process` scan for `0x428C0000`~~ **Resolved statically:** `CCamera::Process` (0x5426668) at 0x5427b1c computes `70.0f / FOV` (`fdiv s0, 70, [x24]`) and stores it to `this+0x104` (`m_fLODDistMultiplier`, same as PC) and `this+0x108`; cutscenes take the `fmov s0,#1.0` branch. LOD port = post-hook `CCamera::Process`, scale `TheCamera+0x104` unless in a cutscene.


## 4b. Probe results (device, v1.87.0; raw log: mods/skygfx/probe_results_v1.87.log)
- **P1:** `GMaxRHIFeatureLevel=3` (SM5) and `GMaxRHIShaderPlatform=0`. The full deferred renderer runs, so SSGI/SSR/volumetric-fog toggles are viable.
- **P2:** at startup, `s_UseLightingOverrides=1` and `s_UseNewNewLightingOverrides=0`. Which one is the Classic Atmosphere flag still needs the in-game toggle: TODO.
- **P3:** `GStreetLightDistance` is an **int32** (16000 at boot, 12000 once in game, so the game sets it from quality). `GStreetlightShadowDistance` is 0 (no lamp shadows).
- **P4:** the `m_CurrentColours` layout matches PC `CColourSet`: ambient floats from 0x00, then shorts (sky top/bottom ...) from 0x24, then floats. The `m_nSkyTopRed` elements are u8.
- **P5:** reflection works.
  - All `AGTATimeOfDay` / `AGTAHeightFog` properties resolve; the offsets are in the log. Examples: TOD `SkylightColor` 0x2C0, `SkyLowerColor` 0x2D0, `SkyUpperColor` 0x2E0, `FogColor` 0x3D8, `SunColor` 0x418, `SkyLightIntensity` 0x4CC, `CloudyAmount` 0x5D4.
  - FOG: `bUseGTAValues` 0x2A4, `HeightFogComponent` 0x2A8, `TimeOfDayActor` 0x2B0.
  - PPS: `ColorGain` 0x60, `BloomIntensity` 0x21C, `GrainIntensity` 0x400. The `bOverride_*` bits are at byte 0x0 / 0x6 / 0xC with masks 0x20 / 0x04 / 0x08.
  - Always resolve these by name at runtime.
- **P6:** `AGTATimeOfDay::{Tick,Update,ApplyValues}`, `AGTAHeightFog::UpdateColors` and `AGTAPostProcessVolume::UpdateColorOptions` each run once per frame. `ApplySettings` runs about 11 times per frame. There is one TOD actor, one fog actor and one PP volume.

## 5. Recommended phase plan

### Phase 0: foundation (S–M)
- AML entry, config load (`GetConfigPath`), `GTAML_OnConfigChanged`, log in the AML data path.
- `dlsym` table; reflection-offset resolver (`FindPropertyByName`) with logged results (probe 5).
- Thread anchor in `AGTATimeOfDay::Tick`.
- The probe set in §4.

### Phase 1: engine-side features portable via symbols
1. PS2 timecyc load (§2.2). S.
2. Weather/time tools via ini keys (§2.15, weather + time part). S.
3. Street-light cvar distances (§2.12, cvar part) and a conservative light-shadow option. S + M.
4. LOD distance (§2.13). S–M (sig scan).
5. Coronas / SALodLights (§2.6). M.
6. Darker shadows (§2.7). S.
7. `r.VolumetricCloud` toggle (§2.5) and Classic-sky scoped flag (§2.4, flag-scope part only). S–M.
8. Colour filter/grade (§2.1). M. This one is render-visible but needs no render code, so it can ship in Phase 1.
9. Timecyc colours for sky/fog/ambient/sun/clouds (§2.3). M–L: do **after** probe 6.

### Phase 2: render-side with alternatives
- Radiosity → UE bloom (§2.8). M.
- Grain → `GrainIntensity` (§2.10). S.
- SpeedFX trigger logic + motion-blur approximation (§2.9). M.
- Fog/Classic details: GTA-fog mode, haze, ground haze, cloud tuning (§2.4). M–L.
- Noclip and freecam (§2.15). M–L.
- Modern look toggles (§2.17), only if SM5.

### Drop
- All D3D11/D3D12/DXGI code, `overlay.cpp`, `postfx.cpp` as such, `OMSetRenderTargets` hooks, the HUD-bind logic and `HudBind` (§2.14).
- ImGui, WndProc/cursor/`GetAsyncKeyState` hooks, hotkey parsing, `[Keys]`, `ToolsWantCapture`.
- Water drops (§2.11) unless a lens-effect asset exists.
- `uobject.cpp` pool walk, `FindUnique`/`RipAt`/`FindIntCVar`/`FindRefCVar`, `ToolsWriteAnchors`, `test/offline_check.cpp` and the e2e scripts.
- The cloud-density immediate patch (§2.4) until `VolumetricCloudsRender` is read.
- SSGI/AO/SSR/volumetric fog on mobile.
- Exact PS2 SpeedFX and radiosity shapes.

## 6. Ini keys for the mods menu

Format is ModMenu-compatible: bool = 0/1; choice = `; N = text` comments; ints step 1; floats step 0.1 with their decimals. Keys that need scaled units are marked `*`. Keys under **[Debug]** are new for Android.

| Section | Key | Type | Default | Notes |
|---|---|---|---|---|
| General | Enabled | bool | 1 | master switch |
| General | Log | choice 0/1/2 | 0 | log level (restart) |
| ColourFilter | PS2Filter | bool | 1 | |
| ColourFilter | Strength | float 0–2 | 1.0 | with a loaded timecyc |
| ColourFilter | GameTimecycStrength | float 0–1 | 0.4 | PC default 0.35; step 0.1 loses 0.35 |
| ColourFilter | KeepBrightness | float 0–1 | 0.5 | |
| Grade | Saturation | float | 1.2 | PC 1.15 |
| Grade | Contrast | float | 1.0 | PC 1.05 |
| Atmosphere | Mode | choice | 0 | 0 = Classic sky, 1 = GTA fog, 2 = modern (replaces `ClassicSky` + `GtaFog`) |
| Atmosphere | FogDistance | float | 1.8 | GTA fog |
| Atmosphere | FogOpacity | float | 0.5 | GTA fog |
| Atmosphere | Haze | float | 0.4 | modern fog; PC 0.35 |
| Atmosphere | GroundHaze | float | 0.0 | |
| Colours | Sky / Fog / Ambient / Sun / Clouds | float 0–1 | 1.0 / 1.0 / 0.3 / 0.4 / 0.5 | PC 0.25 and 0.35 become 0.3 and 0.4 at step 0.1 |
| Colours | Brightness | float 0–1 | 1.0 | |
| Colours | NightExposure | float | -0.5 | EV; set only if mobile exposure honours it |
| Shadows | Darkness | float 0–0.9 | 0.5 | |
| Timecyc | File | string (read-only) | timecyc_ps2.dat | no text input in ModMenu; or `Preset` choice 0 = game, 1 = PS2 |
| Radiosity | Enabled | bool | 1 | maps to bloom retune |
| Radiosity | Intensity | int | 35 | |
| Radiosity | Offset | drop | n/a | no UE equivalent |
| Radiosity | DEBloom | float | 0 | |
| Grain | Enabled | bool | 1 | |
| Grain | Strength | float 0–2 | 1.0 | |
| SpeedFX | Enabled | bool | 1 | approximation |
| SpeedFX | TestMode | bool | 0 | |
| WaterDrops | * | n/a | n/a | dropped |
| StreetLights | DrawDistanceDm* | int | 15 | in tens of metres |
| StreetLights | ShadowDistanceDm* | int | 2 | tens of metres |
| StreetLights | OtherLightShadows | bool | 0 | off by default on a phone |
| StreetLights | BollardBrightness | float | 0.5 | |
| StreetLights | GlobeLampBrightness | int | 8 | PC float 8.0 |
| Corona | Enabled | bool | 1 | |
| Corona | Size | float | 0.5 | |
| Corona | Intensity | float | 1.0 | |
| Corona | FarClipDm* | int | 0 | tens of metres; 0 = auto |
| World | LodDistance | float | 1.8 | 1.0–4.0; cutscenes excluded |
| ModernLook | SSGI/AO/SSR/VolumetricFog + quality levels | bool/int | 0 | only if SM5; otherwise hide or ignore |
| ModernLook | LensFlare, SunStrength | float | 1.0 | |
| Debug | ForceWeather | int | -1 | −1 = game; 0..22 = `kWeatherNames` (`tools.cpp:14-20`), or a Choice list |
| Debug | BlendWeatherB, Blend | int, float | -1, 0 | optional A→B blend |
| Debug | SetHour / SetMinute | int | -1 | one-shot; a write-then-clear key |
| Debug | FreezeTime | bool | 0 | |
| Debug | TimeSpeed | float | 1.0 | 0.1 step; `CClock::ms_nMillisecondsPerGameMinute` / speed |
| Debug | Timelapse | bool | 0 | |
| Debug | Noclip, Freecam | bool | 0 | touch input; see §2.15 |
| Debug | NoclipSpeed, FreecamSpeed | int | 15, 20 | m/s |
| Debug | MovePlayerToCamera | bool | 0 | one-shot |

`PC [Keys]` and `[Tools]` speed sliders: only the speeds survive; the key bindings are dropped.

## 7. Risk summary

| Risk | Where | Mitigation |
|---|---|---|
| Struct offsets differ on arm64 (known drift) | all GTA/UE offsets | reflection-based offsets; verify probes |
| Hook point for sky colours unknown | §2.3 | probe 6 before coding |
| Classic flag identity/polarity | §2.4 | probe 2 |
| Mobile feature level lacks SSGI/SSR/VolFog/clouds | §2.4, §2.5, §2.17 | gate on `GMaxRHIFeatureLevel` |
| Mobile tonemapper vs `FilmToneMap` calibration | §2.1 | tunable strength, per-device presets |
| Phone frame time (lamp shadows, LOD ×1.8) | §2.12, §2.13 | conservative defaults, default-off shadow scan |
| Freecam path (UE view, culling, streaming) | §2.15 | hook both `TheCamera` and the UE view; "move player to camera" |
| UObject calls off the game thread | all `SetCastShadows`-style calls | only from actor-tick hooks |

---
**Skipped / unchecked:** disassembly and string checks of `libUE4.so` (no usable disassembler or binary-aware grep), so every offset/type marked `verify`, every `[INFERENCE]` and the mangled name of `OnClassicAtmosphereSet` are unconfirmed. I did not look up Android names for the water-splash FX wrappers, `FxSystem_c::AddParticle` or the hydrant audio event.
