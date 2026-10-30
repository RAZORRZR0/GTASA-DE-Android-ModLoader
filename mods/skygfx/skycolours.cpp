// SkyGfx timecyc colours: moves AGTATimeOfDay's live sky, fog, ambient, sun and cloud colours towards the hue of the
// timecyc colours (CTimeCycle::m_CurrentColours), with [Colours] strengths and Brightness. Port of skygfxde core.cpp
// (TransferHue, ApplyColours, DayFactor). Feature contract: skygfx.h.
//
// Hook point: Observe(Point::ApplyValues). The PC mod ran ApplyColours in the post-hook of the TOD colour update, the
// function that copies the timecyc target into the live colours. Its Android counterpart is AGTATimeOfDay::ApplyValues
// [INFERENCE, port map §2.3]. A post-hook on it runs after that write, so the transfer overrides the copy the same way.
// Tick is not used: PC's Tick hook only scopes the Classic flag, and the colour write happens inside the colour update.
//
// Not ported here: K calibration per weather. PC measured DE's noon brightness per weather through DE's sky-curve
// evaluator, which has no export on Android. The PC default K values are used for every weather instead.
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "skygfx.h"

namespace skygfx {
namespace {

typedef void* (*StaticFn)();

// CTimeCycle::m_CurrentColours, PC CColourSet offsets (the Android layout is unverified; port map §2.1).
namespace CS {
constexpr size_t Ambient = 0x00;     // float r,g,b
constexpr size_t SkyTop = 0x24;      // uint16 r,g,b
constexpr size_t SkyBottom = 0x2A;   // uint16 r,g,b
constexpr size_t SunCore = 0x30;     // uint16 r,g,b
constexpr size_t LowClouds = 0x5C;   // uint16 r,g,b
}

// The clock is read the same way as colour.cpp (CClock::ms_nGameClockHours/Minutes, exported data).
void* g_todCurrent;   // CTimeCycle::m_CurrentColours
void* g_clockHours;   // CClock::ms_nGameClockHours (uint8)
void* g_clockMinutes; // CClock::ms_nGameClockMinutes (uint8)

// Brightness target per field (PC K, DE's own luminance / timecyc noon luminance on EXTRASUNNY_VEGAS). 0 = hue only.
constexpr float kK_UpperSky = 0.30f, kK_LowerSky = 1.50f, kK_Fog = 0.38f;

enum Src { kAmbient, kSkyTop, kSkyBottom, kSunCore, kLowClouds };

struct Target {
    const char* prop;            // AGTATimeOfDay property (reflection name, probe v1.87)
    Src src;                     // timecyc colour it follows
    float skygfx::Config::*strength;
    float k;                     // brightness target, 0 = hue only
    bool alphaScales;            // sky: rendered as rgb x alpha, alpha moves towards 1 with Brightness
    bool dayBalance;             // sun: scaled by the original's day/night balance
    uint32_t off;                // byte offset in AGTATimeOfDay, from reflection at init
};

Target g_targets[] = {
    { "SkyUpperColor",       kSkyTop,    &skygfx::Config::skyColour,   kK_UpperSky, true,  false, 0 },
    { "SkyLowerColor",       kSkyBottom, &skygfx::Config::skyColour,   kK_LowerSky, true,  false, 0 },
    { "SkylightColor",       kAmbient,   &skygfx::Config::ambient,     0.0f,        false, false, 0 },
    { "FogColor",            kSkyBottom, &skygfx::Config::fogColour,   kK_Fog,      false, false, 0 },
    { "SunColor",            kSunCore,   &skygfx::Config::sunColour,   0.0f,        false, true,  0 },
    { "VolumetricCloudColor", kLowClouds, &skygfx::Config::cloudColour, 0.0f,       false, false, 0 },
};

bool g_ok = false;

// Sun day/night balance (PC DayFactor): day 7:00-20:00, fades 6-7 and 20-21.
float DayFactor(float hours) {
    if (hours < 6.0f || hours >= 21.0f) return 0.0f;
    if (hours < 7.0f) return hours - 6.0f;
    if (hours < 20.0f) return 1.0f;
    return 21.0f - hours;
}

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float ToLinear(float srgb01) { return powf(fminf(fmaxf(srgb01, 0.0f), 1.0f), 2.2f); }

// Moves dst (FLinearColor, alpha kept) towards the hue of the timecyc colour srgb (0..1), keeping dst's luminance.
void TransferHue(float* dst, const float srgb[3], float strength) {
    if (strength <= 0.0f) return;
    const float lin[3] = { ToLinear(srgb[0]), ToLinear(srgb[1]), ToLinear(srgb[2]) };
    const float L = Luma(dst), Lt = Luma(lin);
    for (int i = 0; i < 3; ++i) {
        const float target = Lt > 1e-5f ? L * lin[i] / Lt : L;
        dst[i] += (target - dst[i]) * strength;
    }
}

// Reads the timecyc colour of `src` from m_CurrentColours as 0..1 rgb.
void ReadSource(Src src, float out[3]) {
    const uint8_t* cc = (const uint8_t*)g_todCurrent;
    if (src == kAmbient) {
        for (int i = 0; i < 3; ++i) out[i] = ((const float*)(cc + CS::Ambient))[i];
        return;
    }
    const size_t off = src == kSkyTop ? CS::SkyTop : src == kSkyBottom ? CS::SkyBottom
                     : src == kSunCore ? CS::SunCore : CS::LowClouds;
    for (int i = 0; i < 3; ++i) out[i] = ((const uint16_t*)(cc + off))[i] / 255.0f;
}

void OnApplyValues(void* self, void*, float, float) {
    if (!g_ok || !self || !skygfx::cfg.enabled) return;
    uint8_t* tod = (uint8_t*)self;
    const float hours = (float)*(const uint8_t*)g_clockHours + (float)*(const uint8_t*)g_clockMinutes / 60.0f;
    const float b = cfg.brightness;

    for (const Target& t : g_targets) {
        float* c = (float*)(tod + t.off);
        float colour[3];
        ReadSource(t.src, colour);
        const float strength = cfg.*t.strength;
        TransferHue(c, colour, strength);
        if (t.k > 0.0f && b > 0.0f) {
            const float lin[3] = { ToLinear(colour[0]), ToLinear(colour[1]), ToLinear(colour[2]) };
            // Sky rgb x alpha: alpha moves towards 1 with Brightness, rgb carries the product. The rgb x alpha target
            // can't be reached by dividing by that alpha: it blows up at night (PC fix, see core.cpp ApplyColours).
            const float a = t.alphaScales ? c[3] : 1.0f;
            const float A = t.alphaScales ? fminf(a + (1.0f - a) * b, 1.0f) : 1.0f;
            const float L = Luma(c);
            const float want = fmaxf((L * a + (t.k * Luma(lin) - L * a) * b) / fmaxf(A, 1e-3f), 0.0f);
            const float* hue = L > 1e-5f ? c : lin; // DE black: the timecyc colour gives the hue
            const float s = want / fmaxf(Luma(hue), 1e-5f);
            for (int i = 0; i < 3; ++i) c[i] = fminf(hue[i] * s, 64.0f);
            if (t.alphaScales) c[3] = A;
        }
        if (t.dayBalance) {
            const float day = fmaxf(1.0f + (DayFactor(hours) - 1.0f) * b, 0.0f);
            for (int i = 0; i < 3; ++i) c[i] *= day;
        }
    }
}

bool Resolve() {
    StaticFn stod = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI13AGTATimeOfDayEP6UClassv"));
    g_todCurrent = Sym("_ZN10CTimeCycle16m_CurrentColoursE");
    g_clockHours = Sym("_ZN6CClock18ms_nGameClockHoursE");
    g_clockMinutes = Sym("_ZN6CClock20ms_nGameClockMinutesE");
    if (!stod || !g_todCurrent || !g_clockHours || !g_clockMinutes) return false;
    void* todCls = stod();
    for (Target& t : g_targets)
        if (!FieldOffset(todCls, t.prop, &t.off)) { SKY_LOG("colours: %s not found on AGTATimeOfDay", t.prop); return false; }
    return true;
}

void Init() {
    if (!Resolve()) {
        SKY_LOG("colours: symbols or properties unresolved, timecyc colours disabled");
        return;
    }
    g_ok = true;
    Observe(Point::ApplyValues, OnApplyValues);
    SKY_INFO("colours: ready (SkyUpper %u, SkyLower %u, Skylight %u, Fog %u, Sun %u, Cloud %u)",
             g_targets[0].off, g_targets[1].off, g_targets[2].off, g_targets[3].off, g_targets[4].off, g_targets[5].off);
}

const Feature kFeature = { "colours", Init, nullptr };
const Registrar kRegistrar(kFeature);

}  // namespace
}  // namespace skygfx
