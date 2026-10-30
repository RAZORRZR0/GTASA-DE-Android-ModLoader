// SkyGfx colour: PS2 colour filter gain, grade (saturation/contrast), night exposure, shadow sky light.
// Port of skygfxde core.cpp (ComputePs2Gain, WriteVolume, PerFrame shadow block). Feature contract: skygfx.h.
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "skygfx.h"

namespace skygfx {
namespace {

typedef void* (*StaticFn)();

struct Offsets {
    uint32_t settings = 0;          // APostProcessVolume::Settings
    uint32_t colorGain = 0, colorSat = 0, colorContrast = 0, exposureBias = 0;
    uint32_t film[5] = {};          // FilmSlope, FilmToe, FilmShoulder, FilmBlackClip, FilmWhiteClip
    uint32_t todSkyLight = 0;       // AGTATimeOfDay::SkyLightIntensity
    uint8_t ovGain[2] = {0, 0}, ovSat[2] = {0, 0}, ovContrast[2] = {0, 0}, ovExposure[2] = {0, 0};
    uint8_t ovFilm[5][2] = {};      // bOverride_Film* as (byte, mask), same order as film[]
    bool ok = false;
};
Offsets o;
void* g_settingsStruct;     // UScriptStruct FPostProcessSettings
void* g_todCurrent;         // CTimeCycle::m_CurrentColours (exported B symbol)
void* g_ppVolumeCls;        // UClass APostProcessVolume
void* g_clockHours;         // CClock::ms_nGameClockHours (uint8, exported B symbol)
void* g_clockMinutes;       // CClock::ms_nGameClockMinutes (uint8, exported B symbol)

// Byte offset + mask of a bool override in FPostProcessSettings (FBoolProperty).
bool BoolBit(void* ustruct, const char* name, uint8_t* byte, uint8_t* mask) {
    void* p = FindProperty(ustruct, name);
    if (!p) return false;
    *byte = *((uint8_t*)p + UE::Bool_ByteOffset);  // struct byte of the bool (FBoolProperty)
    *mask = *((uint8_t*)p + UE::Bool_FieldMask);
    return true;
}

bool Resolve() {
    StaticFn sstruct = reinterpret_cast<StaticFn>(Sym("_Z12StaticStructI20FPostProcessSettingsEP13UScriptStructv"));
    StaticFn sppv = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI18APostProcessVolumeEP6UClassv"));
    StaticFn stod = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI13AGTATimeOfDayEP6UClassv"));
    g_todCurrent = Sym("_ZN10CTimeCycle16m_CurrentColoursE");
    g_clockHours = Sym("_ZN6CClock18ms_nGameClockHoursE");
    g_clockMinutes = Sym("_ZN6CClock20ms_nGameClockMinutesE");
    if (!sstruct || !sppv || !stod || !g_todCurrent || !g_clockHours || !g_clockMinutes) return false;
    g_settingsStruct = sstruct();
    g_ppVolumeCls = sppv();
    void* todCls = stod();
    if (!FieldOffset(g_ppVolumeCls, "Settings", &o.settings)) return false;
    static const char* const kFilm[5] = {"FilmSlope", "FilmToe", "FilmShoulder", "FilmBlackClip", "FilmWhiteClip"};
    static const char* const kOvFilm[5] = {"bOverride_FilmSlope", "bOverride_FilmToe", "bOverride_FilmShoulder",
                                           "bOverride_FilmBlackClip", "bOverride_FilmWhiteClip"};
    bool ok = FieldOffset(g_settingsStruct, "ColorGain", &o.colorGain) &&
              FieldOffset(g_settingsStruct, "ColorSaturation", &o.colorSat) &&
              FieldOffset(g_settingsStruct, "ColorContrast", &o.colorContrast) &&
              FieldOffset(g_settingsStruct, "AutoExposureBias", &o.exposureBias) &&
              FieldOffset(todCls, "SkyLightIntensity", &o.todSkyLight);
    for (int k = 0; ok && k < 5; ++k)
        ok = FieldOffset(g_settingsStruct, kFilm[k], &o.film[k]) &&
             BoolBit(g_settingsStruct, kOvFilm[k], &o.ovFilm[k][0], &o.ovFilm[k][1]);
    ok = ok && BoolBit(g_settingsStruct, "bOverride_ColorGain", &o.ovGain[0], &o.ovGain[1]) &&
         BoolBit(g_settingsStruct, "bOverride_ColorSaturation", &o.ovSat[0], &o.ovSat[1]) &&
         BoolBit(g_settingsStruct, "bOverride_ColorContrast", &o.ovContrast[0], &o.ovContrast[1]) &&
         BoolBit(g_settingsStruct, "bOverride_AutoExposureBias", &o.ovExposure[0], &o.ovExposure[1]);
    o.ok = ok;
    return ok;
}

// ---- PS2 colour filter (ComputePs2Gain). UE 4.26 FilmToneMap on grey, then the sRGB encode.
float g_film[5] = {0.88f, 0.55f, 0.26f, 0.0f, 0.04f};  // slope, toe, shoulder, black clip, white clip (UE defaults)

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float FilmDisplay(float x) {
    const float S = g_film[0], Toe = g_film[1], Sh = g_film[2], BC = g_film[3], WC = g_film[4];
    const float TS = 1 + BC - Toe, SS = 1 + WC - Sh;
    float TM;
    if (Toe > 0.8f) TM = (1 - Toe - 0.18f) / S + log10f(0.18f);
    else {
        const float bt = (0.18f + BC) / TS - 1;
        TM = log10f(0.18f) - 0.5f * logf((1 + bt) / (1 - bt)) * (TS / S);
    }
    const float SM = (1 - Toe) / S - TM, ShM = Sh / S - SM;
    if (x <= 0.0f) return 0.0f;
    const float L = log10f(x), st = S * (L + SM);
    const float toe = L < TM ? -BC + 2 * TS / (1 + expf((-2 * S / TS) * (L - TM))) : st;
    const float sho = L > ShM ? (1 + WC) - 2 * SS / (1 + expf((2 * S / SS) * (L - ShM))) : st;
    float t = Clamp((L - TM) / (ShM - TM), 0.0f, 1.0f);
    if (ShM < TM) t = 1 - t;
    t = (3 - 2 * t) * t * t;
    const float v = Clamp(toe + (sho - toe) * t, 0.0f, 1.0f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
}

// ColorGain k that turns mid grey into `display` times its display value.
float LinearGainFor(float display) {
    const float target = fminf(display * FilmDisplay(0.18f), 0.999f);
    float lo = 1e-4f, hi = 64.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid = sqrtf(lo * hi);
        (FilmDisplay(mid) < target ? lo : hi) = mid;
    }
    return lo / 0.18f;
}

// CColourSet layout (verify on device): PostFx1 +0x78, PostFx2 +0x88 (float r,g,b,a 0..255), ShadowStrength +0x48 (i16).
constexpr uint32_t kPostFx1 = 0x78, kPostFx2 = 0x88, kShadowStrength = 0x48;

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

void ComputePs2Gain(const uint8_t* cc, float gain[3]) {
    const float* c1 = (const float*)(cc + kPostFx1);
    const float* c2 = (const float*)(cc + kPostFx2);
    const float a2 = fminf(1.0f, 2.0f * c2[3] / 255.0f);
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = 2.0f * c1[i] / 255.0f + 2.0f * c2[i] / 255.0f * a2;
    const float l = Luma(g);
    // As PC: full filter only with a loaded (PS2) timecyc; the game's own timecyc has strong mobile-style postfx
    // colours that would give a heavily orange gain, so there it's tint-only at GameTimecycStrength.
    const bool custom = TimecycApplied();
    const float keep = custom ? cfg.keepBrightness : 1.0f;
    const float strength = custom ? cfg.filterStrength : cfg.gameTimecycStrength;
    const float norm = l > 1e-3f ? powf(l, keep) : 1.0f;
    for (int i = 0; i < 3; ++i)
        gain[i] = Clamp(LinearGainFor(fmaxf(1.0f + (g[i] / norm - 1.0f) * strength, 0.01f)), 0.0f, 4.0f);
}

// ---- per-volume DE snapshot (DE rewrites colour options each call; exposure bias is kept by DE only).
struct VolSnap { void* obj; float origBias; };
VolSnap g_vols[8];
int g_volCount;

float* OriginalBias(void* obj) {
    for (int i = 0; i < g_volCount; ++i)
        if (g_vols[i].obj == obj) return &g_vols[i].origBias;
    if (g_volCount >= 8) return nullptr;
    VolSnap& v = g_vols[g_volCount++];
    v.obj = obj;
    v.origBias = *(float*)((uint8_t*)obj + o.settings + o.exposureBias);
    return &v.origBias;
}

// PC DayFactor: day/night balance over the game hour.
float DayFactor(float hours) {
    if (hours < 6.0f || hours >= 21.0f) return 0.0f;
    if (hours < 7.0f) return hours - 6.0f;
    if (hours < 20.0f) return 1.0f;
    return 21.0f - hours;
}

// 1 at night, 0 by day: PC g_night = 1 - DayFactor(hours).
float NightWeight() {
    const float h = (float)*(const uint8_t*)g_clockHours + (float)*(const uint8_t*)g_clockMinutes / 60.0f;
    return 1.0f - DayFactor(h);
}

// Interior flag on AGTAPostProcessVolume (PC offset; verify on device).
constexpr uint32_t kIsInterior = 0x864;

// Scales the grade, filter gain and night exposure into one PostProcessVolume (PC WriteVolume).
void OnPPUpdate(void* self, void*, float, float) {
    if (!o.ok || !self || !cfg.enabled) return;
    uint8_t* s = (uint8_t*)self + o.settings;
    const uint8_t* cc = (const uint8_t*)g_todCurrent;
    const bool interior = *((uint8_t*)self + kIsInterior) != 0;

    // Exterior film curve feeds the PS2 gain (PC TrackVolume).
    if (!interior)
        for (int k = 0; k < 5; ++k)
            if (s[o.ovFilm[k][0]] & o.ovFilm[k][1]) g_film[k] = *(float*)(s + o.film[k]);

    // DE's base value for a vector we scale: if the volume still holds our last output, DE didn't rewrite it this
    // call, so reuse the base we scaled before (otherwise the multiply would compound every frame).
    struct VecMem { void* obj; float base[3][3], written[3][3]; };
    static VecMem mem[8];
    VecMem* m = nullptr;
    for (VecMem& v : mem) if (v.obj == self) { m = &v; break; }
    if (!m) for (VecMem& v : mem) if (!v.obj) { v.obj = self; m = &v; memset(v.written, 0xFF, sizeof v.written); break; }
    if (!m) return;
    auto scale = [&](int slot, float* cur, const float* f) {
        if (memcmp(cur, m->written[slot], sizeof m->written[slot]) == 0) memcpy(cur, m->base[slot], sizeof m->base[slot]);
        else memcpy(m->base[slot], cur, sizeof m->base[slot]);
        for (int i = 0; i < 3; ++i) cur[i] *= f[i];
        memcpy(m->written[slot], cur, sizeof m->written[slot]);
    };
    // Colour filter (PS2) on DE's colour gain.
    if (cfg.ps2Filter) {
        float g[3];
        ComputePs2Gain(cc, g);
        scale(0, (float*)(s + o.colorGain), g);
        s[o.ovGain[0]] |= o.ovGain[1];
    }
    // Grade.
    {
        const float sat[3] = {cfg.saturation, cfg.saturation, cfg.saturation};
        scale(1, (float*)(s + o.colorSat), sat);
        s[o.ovSat[0]] |= o.ovSat[1];
        const float con[3] = {cfg.contrast, cfg.contrast, cfg.contrast};
        scale(2, (float*)(s + o.colorContrast), con);
        s[o.ovContrast[0]] |= o.ovContrast[1];
    }
    // Night exposure on exterior volumes only (PC WriteVolume).
    if (!interior && (s[o.ovExposure[0]] & o.ovExposure[1])) {
        float* bias = OriginalBias(self);
        if (bias) *(float*)(s + o.exposureBias) = *bias + cfg.nightExposure * NightWeight();
    }
}

// Shadow sky light: scale SkyLightIntensity by 1 - Darkness * shadowStrength / 255, restoring DE's value.
void OnTodUpdate(void* self, void*, float, float) {
    static float* skyP = nullptr;
    static float skyDE = 0.0f, skyWritten = NAN;
    if (!o.ok || !self) return;
    float* sp = (float*)((uint8_t*)self + o.todSkyLight);
    if (sp != skyP) { if (skyP) *skyP = skyDE; skyP = sp; skyDE = *sp; skyWritten = NAN; }
    if (*sp != skyWritten) skyDE = *sp;
    const float shadowStrength = (float)*(const int16_t*)((const uint8_t*)g_todCurrent + kShadowStrength);
    const float indirect = Clamp(1.0f - cfg.shadowDarkness * shadowStrength / 255.0f, 0.1f, 1.0f);
    skyWritten = skyDE * indirect;
    *sp = skyWritten;
}

void Init() {
    if (!Resolve()) {
        SKY_LOG("colour: reflection/symbols unresolved, colour features disabled");
        return;
    }
    Observe(Point::PPUpdateColorOptions, OnPPUpdate);
    Observe(Point::ApplyValues, OnTodUpdate);
    SKY_INFO("colour: ready (gain %u, sat %u, exposure %u, skylight %u)",
             o.colorGain, o.colorSat, o.exposureBias, o.todSkyLight);
}

const Feature kFeature = {"colour", Init, nullptr};
const Registrar kRegistrar(kFeature);

}  // namespace
}  // namespace skygfx
