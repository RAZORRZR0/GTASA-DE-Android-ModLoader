// SkyGfx postfx: radiosity (as UE bloom) and PS2 rain grain (as UE grain), written into each PostProcessVolume.
// Port of skygfxde postfx.cpp (Radiosity, Grain) and tools.cpp (rain grain state). Feature contract: skygfx.h.
#include <math.h>
#include <stdint.h>
#include "skygfx.h"

namespace skygfx {
namespace {

typedef void* (*StaticFn)();

constexpr uint32_t kHighLightMinIntensity = 0x9C;  // CColourSet::m_nHighLightMinIntensity (PC postfx.cpp:238)
constexpr float kPcCameraZLimit = 900.0f;          // PC tools.cpp: no grain above 900 m

struct Offsets {
    uint32_t settings = 0;  // APostProcessVolume::Settings
    uint32_t bloomIntensity = 0, bloomThreshold = 0, grainIntensity = 0, grainJitter = 0;
    uint8_t ovBloomI[2] = {0, 0}, ovBloomT[2] = {0, 0}, ovGrainI[2] = {0, 0}, ovGrainJ[2] = {0, 0};
    bool ok = false;
};
Offsets o;

const uint8_t* g_curColours;      // CTimeCycle::m_CurrentColours
const float* g_rain;              // CWeather::Rain
const float* g_underWater;        // CWeather::UnderWaterness
const int32_t* g_currArea;        // CGame::currArea (0 = outside)
const uint32_t* g_zoneFlagsP;     // CCullZones::CurrentFlags_Player (bit 3 = no rain)
const uint32_t* g_zoneFlagsC;     // CCullZones::CurrentFlags_Camera (bit 3 = no rain)
const uint8_t* g_camera;          // TheCamera; +0x18 holds CMatrix*
const uint32_t* g_timeMs;         // CTimer::m_snTimeInMilliseconds (changes once per frame)

// Byte offset + mask of a bool override in FPostProcessSettings (FBoolProperty). Same as colour.cpp.
bool BoolBit(void* ustruct, const char* name, uint8_t* byte, uint8_t* mask) {
    void* p = FindProperty(ustruct, name);
    if (!p) return false;
    *byte = *((uint8_t*)p + UE::Bool_ByteOffset);
    *mask = *((uint8_t*)p + UE::Bool_FieldMask);
    return true;
}

bool Resolve() {
    StaticFn sstruct = reinterpret_cast<StaticFn>(Sym("_Z12StaticStructI20FPostProcessSettingsEP13UScriptStructv"));
    StaticFn sppv = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI18APostProcessVolumeEP6UClassv"));
    g_curColours = (const uint8_t*)Sym("_ZN10CTimeCycle16m_CurrentColoursE");
    g_rain = (const float*)Sym("_ZN8CWeather4RainE");
    g_underWater = (const float*)Sym("_ZN8CWeather14UnderWaternessE");
    g_currArea = (const int32_t*)Sym("_ZN5CGame8currAreaE");
    g_zoneFlagsP = (const uint32_t*)Sym("_ZN10CCullZones19CurrentFlags_PlayerE");
    g_zoneFlagsC = (const uint32_t*)Sym("_ZN10CCullZones19CurrentFlags_CameraE");
    g_camera = (const uint8_t*)Sym("TheCamera");
    g_timeMs = (const uint32_t*)Sym("_ZN6CTimer22m_snTimeInMillisecondsE");
    if (!sstruct || !sppv || !g_curColours || !g_rain || !g_underWater || !g_currArea || !g_zoneFlagsP ||
        !g_zoneFlagsC || !g_camera || !g_timeMs) return false;
    void* settingsStruct = sstruct();
    void* ppvCls = sppv();
    bool ok = FieldOffset(ppvCls, "Settings", &o.settings) &&
              FieldOffset(settingsStruct, "BloomIntensity", &o.bloomIntensity) &&
              FieldOffset(settingsStruct, "BloomThreshold", &o.bloomThreshold) &&
              FieldOffset(settingsStruct, "GrainIntensity", &o.grainIntensity) &&
              FieldOffset(settingsStruct, "GrainJitter", &o.grainJitter);
    ok = ok && BoolBit(settingsStruct, "bOverride_BloomIntensity", &o.ovBloomI[0], &o.ovBloomI[1]) &&
         BoolBit(settingsStruct, "bOverride_BloomThreshold", &o.ovBloomT[0], &o.ovBloomT[1]) &&
         BoolBit(settingsStruct, "bOverride_GrainIntensity", &o.ovGrainI[0], &o.ovGrainI[1]) &&
         BoolBit(settingsStruct, "bOverride_GrainJitter", &o.ovGrainJ[0], &o.ovGrainJ[1]);
    o.ok = ok;
    return ok;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Radiosity: highlights above limit = hl * 128 / 255 are added back at alpha Intensity/255 (PC postfx.cpp:238).
// UE bloom: threshold = limit / 255; intensity = DE bloom * DEBloom + Intensity / 255.
void WriteRadiosity(uint8_t* s) {
    const int limit = *(const uint8_t*)(g_curColours + kHighLightMinIntensity) * 128 / 255;
    float* bi = (float*)(s + o.bloomIntensity);
    // DE's bloom: if the field still holds our last output, DE didn't rewrite it, so reuse the stored base.
    struct Mem { uint8_t* s; float base, written; };
    static Mem mem[8];
    Mem* m = nullptr;
    for (Mem& e : mem) if (e.s == s || !e.s) { if (!e.s) e = {s, 0.0f, NAN}; m = &e; break; }
    if (!m) return;
    if (*bi != m->written) m->base = *bi;
    *bi = m->base * cfg.radiosityDeBloom + cfg.radiosityIntensity / 255.0f;
    m->written = *bi;
    *(float*)(s + o.bloomThreshold) = limit / 255.0f;
    s[o.ovBloomI[0]] |= o.ovBloomI[1];
    s[o.ovBloomT[0]] |= o.ovBloomT[1];
}

// Rain grain mask (PC SpeedFxRowUpdate): strength steps 1 per game frame toward 128 * rain; mask = strength / 4
// outside no-rain zones, above water, outside (currArea 0) and below 900 m. Stepped once per game frame.
int GrainMask() {
    static uint32_t lastMs = 0;
    static int strength = 0, mask = 0;
    if (*g_timeMs == lastMs) return mask;
    lastMs = *g_timeMs;
    const float rain = *g_rain;
    mask = 0;
    if (rain > 0.0f || strength != 0) {
        if ((float)strength < 128.0f * rain) ++strength;
        else if ((float)strength > 128.0f * rain) --strength;
        if (strength < 0) strength = 0;
        const float* m = *(const float* const*)(g_camera + 0x18);
        const bool noRainZone = ((*g_zoneFlagsP | *g_zoneFlagsC) & 8) != 0;
        if (m && !noRainZone && *g_underWater <= 0.0f && *g_currArea == 0 && m[14] <= kPcCameraZLimit) mask = strength / 4;
    }
    return mask;
}

// Grain: PC blends dst * (1 + 2 * alpha) with alpha = (rand & mask) / 255 * grainStrength, so the mean is
// mask / 255 * 2 * grainStrength. Jitter 1 matches PC regenerating the noise every frame.
void WriteGrain(uint8_t* s, int mask) {
    *(float*)(s + o.grainIntensity) = Clamp(2.0f * mask / 255.0f * cfg.grainStrength, 0.0f, 1.0f);
    *(float*)(s + o.grainJitter) = 1.0f;
    s[o.ovGrainI[0]] |= o.ovGrainI[1];
    s[o.ovGrainJ[0]] |= o.ovGrainJ[1];
}

void OnPPUpdate(void* self, void*, float, float) {
    if (!o.ok || !self || !cfg.enabled) return;
    uint8_t* s = (uint8_t*)self + o.settings;
    const int mask = GrainMask();  // steps every frame, like PC, even while grain is off
    if (cfg.radiosity) WriteRadiosity(s);
    if (cfg.grain) WriteGrain(s, mask);
}

void Init() {
    if (!Resolve()) {
        SKY_LOG("postfx: reflection/symbols unresolved, radiosity and grain disabled");
        return;
    }
    Observe(Point::PPUpdateColorOptions, OnPPUpdate);
    SKY_INFO("postfx: ready (bloom %u/%u, grain %u/%u)", o.bloomIntensity, o.bloomThreshold, o.grainIntensity,
             o.grainJitter);
}

const Feature kFeature = {"postfx", Init, nullptr};
const Registrar kRegistrar(kFeature);

}  // namespace
}  // namespace skygfx
