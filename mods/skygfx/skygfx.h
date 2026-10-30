// SkyGfx Android core: the contract between core.cpp and feature files.
//
// Rules for feature files:
//  - One feature per .cpp. Register it with a static Registrar. Do not touch static init.
//  - Do not define MYMOD, NEEDGAME or ON_MOD_LOAD. core.cpp owns them.
//  - Resolve engine symbols and install hooks only from Feature::init (libUE4 is loaded by then).
//  - Only UObject access from the game thread (see OnGameThread). configChanged runs on the ModMenu thread, so it must only set state that Tick/Update consume.
//  - One-shot Debug keys (SetHour, SetMinute, MovePlayerToCamera) are consumed in memory by their feature (reset to -1 or 0). Core never writes SkyGfx.ini.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

// Tag "SkyGfx". SKY_LOG always prints. SKY_INFO and SKY_VERBOSE are gated by [General] Log.
#define SKY_LOG(...) __android_log_print(ANDROID_LOG_INFO, "SkyGfx", __VA_ARGS__)
#define SKY_INFO(...) do { if (skygfx::cfg.log >= 1) SKY_LOG(__VA_ARGS__); } while (0)
#define SKY_VERBOSE(...) do { if (skygfx::cfg.log >= 2) SKY_LOG(__VA_ARGS__); } while (0)

namespace skygfx {

// Settings from SkyGfx.ini. Field names map to [Section] Key in the ini. Defaults are the Android defaults.
// Core reloads the struct on load and after every ModMenu save. Features read fields directly.
struct Config {
    // [General]
    bool enabled = true;              // Enabled
    int log = 0;                      // Log: 0 errors/startup, 1 info, 2 verbose.

    // [ColourFilter]
    bool ps2Filter = true;            // PS2Filter
    float filterStrength = 1.0f;      // Strength
    float gameTimecycStrength = 0.4f; // GameTimecycStrength
    float keepBrightness = 0.5f;      // KeepBrightness

    // [Grade]
    float saturation = 1.2f;          // Saturation
    float contrast = 1.0f;            // Contrast

    // [Atmosphere]
    int atmosphereMode = 0;           // Mode: 0 classic sky, 1 GTA fog, 2 modern
    float fogDistance = 1.8f;         // FogDistance
    float fogOpacity = 0.5f;          // FogOpacity
    float haze = 0.4f;                // Haze
    float groundHaze = 0.0f;          // GroundHaze

    // [Colours]
    float skyColour = 1.0f;           // Sky
    float fogColour = 1.0f;           // Fog
    float ambient = 0.3f;             // Ambient
    float sunColour = 0.4f;           // Sun
    float cloudColour = 0.5f;         // Clouds
    float brightness = 1.0f;          // Brightness
    float nightExposure = -0.5f;      // NightExposure

    // [Shadows]
    float shadowDarkness = 0.5f;      // Darkness

    // [Timecyc]
    bool ps2Timecyc = true;           // PS2Timecyc: 1 PS2 timecyc table, 0 game table

    // [Radiosity]
    bool radiosity = true;            // Radiosity
    int radiosityIntensity = 35;      // Intensity
    float radiosityDeBloom = 0.0f;    // DEBloom

    // [Grain]
    bool grain = true;                // Grain
    float grainStrength = 1.0f;       // GrainStrength

    // [SpeedFX]
    bool speedFx = true;              // SpeedFX
    bool speedFxTest = false;         // TestMode

    // [StreetLights]
    int streetLightDrawDm = 15;       // DrawDistanceDm
    int streetLightShadowDm = 2;      // ShadowDistanceDm
    bool otherLightShadows = false;   // OtherLightShadows
    float bollardBrightness = 0.5f;   // BollardBrightness
    int globeLampBrightness = 8;      // GlobeLampBrightness

    // [Corona]
    bool corona = true;               // Enabled
    float coronaSize = 0.5f;          // Size
    float coronaIntensity = 1.0f;     // Intensity
    int coronaFarClipDm = 0;          // FarClipDm (0 = auto)

    // [World]
    float lodDistance = 1.8f;         // LodDistance

    // [ModernLook] (only effective when GMaxRHIFeatureLevel == 3)
    bool ssgi = false;                // SSGI
    int ssgiQuality = 4;              // SSGIQuality
    bool ambientOcclusion = false;    // AmbientOcclusion
    bool reflections = false;         // Reflections
    int reflectionQuality = 4;        // ReflectionQuality
    bool volumetricFog = false;       // VolumetricFog
    bool lensFlare = true;            // LensFlare
    float sunStrength = 1.0f;         // SunStrength

    // [Debug]
    int forceWeather = -1;            // ForceWeather (-1 off, 0..22)
    int blendWeatherB = -1;           // BlendWeatherB (-1 off, 0..22)
    float blend = 0.0f;               // Blend (0..1 between weather A and B)
    int setHour = -1;                 // SetHour (-1 off, one-shot)
    int setMinute = -1;               // SetMinute (-1 off, one-shot)
    bool freezeTime = false;          // FreezeTime
    float timeSpeed = 1.0f;           // TimeSpeed
    bool timelapse = false;           // Timelapse
    bool noclip = false;              // Noclip
    bool freecam = false;             // Freecam
    int noclipSpeed = 15;             // NoclipSpeed
    int freecamSpeed = 20;            // FreecamSpeed
    bool movePlayerToCamera = false;  // MovePlayerToCamera (one-shot)
    bool probes = false;              // Probes: 1 run probes.cpp (restart required; hooks install at load)
};
extern Config cfg;

// Resolves a symbol in libUE4.so through aml. Logs "MISSING <name>" and returns nullptr when absent.
// Valid from Feature::init onward.
void* Sym(const char* name);

// Reflection offsets for UE 4.26. Verified against device v1.87.0 by the probes (port map §4b).
namespace UE {
constexpr size_t Struct_Props = 0x70;                                   // UStruct::PropertyLink
constexpr size_t Field_Name = 0x28, Field_Class = 0x08;                 // FField
constexpr size_t Prop_LinkNext = 0x58, Prop_ElemSize = 0x38, Prop_Offset = 0x4C;  // FProperty
constexpr size_t Bool_ByteOffset = 0x79, Bool_FieldMask = 0x7B;         // FBoolProperty
struct FNameRaw { int32_t Index, Number; };
}

// Finds a property by name on a UStruct or UClass. Returns null when it is absent.
void* FindProperty(void* ustruct, const char* name);
// Reads the byte offset of a named property into *outOffset. False when the property is absent.
bool FieldOffset(void* ustruct, const char* name, uint32_t* outOffset);

// Live actors, refreshed by the core hooks before observers run. Null until the first call. Exactly one of each in game.
void* TimeOfDay();          // AGTATimeOfDay*
void* HeightFog();          // AGTAHeightFog*
void* PostProcessVolume();  // AGTAPostProcessVolume*

// GMaxRHIFeatureLevel, read on each call because the RHI is created after mod load. 3 = SM5. -1 when unreadable.
int RhiFeatureLevel();

// True on the thread that runs AGTATimeOfDay::Tick. UObject access is only safe there.
bool OnGameThread();

// Points where core hooks the game and fans out to observers. Observers run after the original returns.
// Args after `self`: ApplyValues/UpdateColorOptions give none. ApplySettings sends (FSkySettings*, a, b).
// Update, Tick and FogUpdateColors send a = dt. ApplySettings runs about 11 times per frame, the others once.
enum class Point : int { ApplyValues, ApplySettings, Update, Tick, FogUpdateColors, PPUpdateColorOptions, kCount };
typedef void (*ObserveFn)(void* self, void* arg, float a, float b);
// Adds an observer. Call from Feature::init only. Up to 8 per point; extras are logged and dropped.
void Observe(Point point, ObserveFn fn);
// Same points, but the observer runs BEFORE the original (scoped flags: set in Pre, restore in Observe).
void ObservePre(Point point, ObserveFn fn);

// Console variables (game thread). Slots verified from FConsoleVariable<int>'s vtable in libUE4.so (UE 4.26
// IConsoleVariable: Set=17, GetInt=19, GetFloat=20). Values are set with ECVF_SetByCode.
void* FindCVar(const char* name);                  // nullptr if the cvar doesn't exist
bool SetCVarInt(const char* name, int value);
bool SetCVarFloat(const char* name, float value);
int GetCVarInt(const char* name, int def);
float GetCVarFloat(const char* name, float def);

// A feature file registers one of these from a static Registrar.
// init: once, after symbols resolve and before the core hooks install. Install hooks and observers here.
// configChanged: after init and after every ModMenu save. Re-apply engine state from cfg here.
// Either may be null.
// init runs on the first game-thread AGTATimeOfDay::Tick, when the UObject system is up (StaticClass/StaticStruct
// are safe), unless early: early features init at mod load, so they may only resolve symbols and install hooks
// (needed for hooks that must precede the game's own init, e.g. CTimeCycle::Initialise, IPL loading).
struct Feature {
    const char* name;
    void (*init)();
    void (*configChanged)();
    bool early = false;
};
struct Registrar {
    explicit Registrar(const Feature& feature);
};

}  // namespace skygfx

namespace skygfx {
// True while a custom (PS2) timecyc table is actually loaded into the game (PC g_customTimecyc).
bool TimecycApplied();
}  // namespace skygfx

// Called by ModMenu after it saves SkyGfx.ini (dlsym). Re-reads cfg and calls every feature's configChanged.
extern "C" void GTAML_OnConfigChanged();
