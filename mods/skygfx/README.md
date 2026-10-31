# SkyGfx (Android port of SkyGfxDE)

AML mod for GTA SA Definitive Edition (UE 4.26, arm64). Logcat tag: `SkyGfx`.

## Layout

| File | Role |
|---|---|
| `skygfx.h` | Public contract: `Config`, `Sym`, `FindProperty`, `Observe`, `Feature`/`Registrar`, `UE` offsets. Read it before writing a feature. |
| `core.cpp` | Entry point (`MYMOD`, `ON_MOD_LOAD`), `SkyGfx.ini` parser, engine symbol lookup, actor hook fan-out, `GTAML_OnConfigChanged`. |
| `probes.cpp` | On-device diagnostics. Built always, runs only when `[Debug] Probes=1` (also makes `vkhook.cpp` log the Vulkan frame structure, `P7`). |
| `vkhook.cpp` | Intercepts UE's Vulkan RHI (loader `vkGet*ProcAddr`, wrappers for libUE4.so only); finds the scene pass before the HUD; shadows and replays UE's command-buffer state around the effects. |
| `vkfx.cpp`, `shaders/` | The PC post effects as Vulkan passes on that frame: water drops, SpeedFX, radiosity, PS2 grain (PC `postfx.cpp`). `shaders/build.sh` regenerates `shaders_spv.h`. |
| `fxstate.cpp`, `fx.h` | Game-thread inputs for the effects (SpeedFX row, rain grain, water-drop state and sources), handed to the render thread. |
| `SkyGfx.ini` | Settings, shown by ModMenu. Defaults match `Config` in `skygfx.h` and PC SkyGfxDE. |

Features are added as their own `.cpp` files that register with `static skygfx::Registrar`. They read `skygfx::cfg` and subscribe to actor functions with `skygfx::Observe`. They do not define `MYMOD` or `ON_MOD_LOAD`.

Settings apply live, on the game thread: ModMenu calls `GTAML_OnConfigChanged()` after saving, and core also watches `SkyGfx.ini` (mtime/size, about once a second), so an edited or pushed file applies without the menu. Both only flag a reload; the next `AGTATimeOfDay::Tick` reads the ini and calls every feature's `configChanged`. Two exceptions need a restart: `[Debug] Probes` (its hooks install at load) and any change that a feature latches in its own `init`.

`[General] Log=2` adds one status line per feature every ~150 ticks (`coronas: lamps=… shown=…`, `speedfx: row=… MotionBlurQuality=…`, `lights: draw=… maxdraw=…`, `tools: clock=… player=… ppos=… cpos=…`).

## Not on Android

DE Android runs Unreal's mobile forward renderer (`r.Mobile.ShadingPath=0`), built without the desktop features some PC
SkyGfxDE settings drive, so these PC keys have no Android counterpart:

- `[StreetLights] OtherLightShadows`, `ShadowDistance`: movable spot/point light shadows are compiled out
  (`r.Mobile.EnableMovableSpotlightsShadow=0`). Forcing the cvar at load, with every lamp casting and DE's streetlight
  shadow switch on, still draws no lamp shadow.
- `[ModernLook]` SSGI, AmbientOcclusion, Reflections, VolumetricFog: desktop-renderer passes.
- `[ModernLook] LensFlare`: only the desktop `AddPostProcessingPasses` calls `AddLensFlaresPass`; the mobile chain has none.
- `[Keys]`, the ImGui overlay, `[SpeedFX] HudBind`: ModMenu and the live ini reload replace them.

## Device test

`test/device_test.py` builds, deploys, launches the game and gets into gameplay by itself (intros, Rockstar sign-in skipped, the repack's "While in development" dialog answered with Normal Action, Resume), then runs scenarios by editing the ini live and asserting on status lines and screenshots (`/tmp/skygfx_test`). It restores the user's ini at the end, with the one-shot keys cleared. Needs adb root on the device and a save to resume.

| Scenario | Asserts |
|---|---|
| boot | no crash, no `MISSING`/self-check/hook failures, lamps caught from IPL loading |
| time | 12:00 then 23:30 by `SetHour`; the sky 2 s after the jump is under half its noon luminance; `FreezeTime` holds |
| coronas | coronas shown at night; removed when `[Corona] Enabled=0`, back at 1 |
| lights | `GStreetLightDistance` set; a live lamp's `MaxDrawDistance` follows |
| sun | `[ModernLook] SunStrength` 3 brightens the frozen midday frame over 1 |
| speedfx | `TestMode` writes row 6, turns the motion blur pass on, and lowers the sharpness of the screen sides in a freecam flight below 85 % of the same flight without it; DE's values come back |
| freecam | a 3 s stick hold moves GTA's camera and the rendered view (frame centre changes), not the player |
| noclip | a 3 s stick hold moves the player; no fall after release |

`python3 test/device_test.py [--only time,coronas] [--no-build] [--no-launch]`

## Probes

Set `[Debug] Probes=1` and restart the game. Each probe logs lines prefixed `P<n>`.

| Probe | What it logs | When |
|---|---|---|
| P1 | `GMaxRHIFeatureLevel`, `GMaxRHIShaderPlatform` (raw int) | load, +20 s |
| P2 | `s_UseLightingOverrides`, `s_UseNewNewLightingOverrides` bytes | polled every 500 ms; start and on change |
| P3 | `GStreetLightDistance`, `GStreetlightShadowDistance` (float + raw hex) | load, +60 s |
| P4 | hex of `CTimeCycle::m_CurrentColours` (0xB8) and `m_nSkyTopRed` (16 B) | +60 s |
| P5 | `FindPropertyByName` offset/size/class for named props, all props of AGTATimeOfDay and AGTAHeightFog, bool masks of FPostProcessSettings | first ProcessEvent after class getters resolve; fallback +20 s |
| P6 | call counts and first `this` for six hooked functions | every 5 s |

Notes:
- P5 bool offsets (`+0x79` byte, `+0x7B` mask) and the FProperty/UStruct links are inferred from `docs/reflection_layout.md`. Check them against the P5 output.
- Missing symbols log `MISSING <sym>`. Only the core reflection symbols abort the mod.
