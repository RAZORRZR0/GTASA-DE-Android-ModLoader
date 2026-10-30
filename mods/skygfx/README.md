# SkyGfx (Android port of SkyGfxDE)

AML mod for GTA SA Definitive Edition (UE 4.26, arm64). Logcat tag: `SkyGfx`.

## Layout

| File | Role |
|---|---|
| `skygfx.h` | Public contract: `Config`, `Sym`, `FindProperty`, `Observe`, `Feature`/`Registrar`, `UE` offsets. Read it before writing a feature. |
| `core.cpp` | Entry point (`MYMOD`, `ON_MOD_LOAD`), `SkyGfx.ini` parser, engine symbol lookup, actor hook fan-out, `GTAML_OnConfigChanged`. |
| `probes.cpp` | On-device diagnostics. Built always, runs only when `[Debug] Probes=1`. |
| `SkyGfx.ini` | Settings, shown by ModMenu. Defaults match `Config` in `skygfx.h`. |

Features are added as their own `.cpp` files that register with `static skygfx::Registrar`. They read `skygfx::cfg` and subscribe to actor functions with `skygfx::Observe`. They do not define `MYMOD` or `ON_MOD_LOAD`.

Settings apply live: ModMenu calls `GTAML_OnConfigChanged()` after saving. Two exceptions need a restart: `[Debug] Probes` (its hooks install at load) and any change that a feature latches in its own `init`.

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
