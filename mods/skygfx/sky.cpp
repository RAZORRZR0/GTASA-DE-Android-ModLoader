// Classic sky flag scope, classic density cap and GTA height-fog density for the Android UE4.26 build.
// Ported from SkyGfxDE core.cpp (Hooked_FogUpdateColors, classic flag scope).
#include <math.h>
#include <stdint.h>
#include "skygfx.h"

using namespace skygfx;

namespace {

// Classic flag: AGTATimeOfDay::s_UseLightingOverrides (device-verified: toggling the in-game option flips it 1<->0).
const char* const kClassicFlagSym = "_ZN13AGTATimeOfDay22s_UseLightingOverridesE";
const char* const kColoursSym = "_ZN10CTimeCycle16m_CurrentColoursE";

uint8_t* g_classicFlag = nullptr;
const uint8_t* g_colours = nullptr;   // CTimeCycle::m_CurrentColours (CColourSet layout, device-verified: FarClip 0x50, FogStart 0x54)
int g_depth = 0;                      // nesting depth of scoped Tick / UpdateColors calls
bool g_held = false;                  // we set the flag to 1 for the outermost scope
uint8_t g_saved = 0;                  // game's own flag value, restored when the outermost scope ends
uint32_t g_offDensity = 0;
bool g_offOk = false;

// PC constants, kept verbatim.
constexpr double kLn2 = 0.693147;
constexpr double kGtaScale = 0.480453;
constexpr double kClassicFarMul = 1.8;

float FarClip() { return *reinterpret_cast<const float*>(g_colours + 0x50); }
float FogStartM() { return *reinterpret_cast<const float*>(g_colours + 0x54); }

// Scoped classic flag: held at 1 for the outermost Tick / UpdateColors call while atmosphereMode==0.
// Depth counts both kinds of call, so a nested call restores nothing. Pre and post always pair.
void ScopePre(void*, void*, float, float) {
    if (!g_classicFlag) return;
    if (g_depth++ == 0 && cfg.atmosphereMode == 0) {
        g_saved = *g_classicFlag;
        *g_classicFlag = 1;
        g_held = true;
    }
}

void ScopePost(void*, void*, float, float) {
    if (!g_classicFlag) return;
    if (--g_depth == 0 && g_held) {
        *g_classicFlag = g_saved;
        g_held = false;
    }
}

// Post-FogUpdateColors: overwrite BaseFogDensity with the PC classic cap or the GTA density.
// shortcut: PC restores DE's cached density before the original runs; the post-only result is recomputed here instead.
void OnFogUpdateColors(void* self, void*, float, float) {
    if (!self || !g_colours || !OnGameThread()) return;
    uint8_t* fog = static_cast<uint8_t*>(self);
    void* cls = *reinterpret_cast<void**>(fog + 0x10); // UObject::ClassPrivate
    if (!g_offOk) {
        g_offOk = FieldOffset(cls, "BaseFogDensity", &g_offDensity);
        if (!g_offOk) { SKY_LOG("MISSING AGTAHeightFog.BaseFogDensity"); return; }
    }
    float* density = reinterpret_cast<float*>(fog + g_offDensity);
    const double far = FarClip();
    const double start = FogStartM();

    if (cfg.atmosphereMode == 0) {
        // Classic cap: ln2*1000 / (scale * half * 100), half = 0.5*(start + far*1.8)
        const double half = 0.5 * (start + far * kClassicFarMul);
        const double cap = kLn2 * 1000.0 / (kGtaScale * half * 100.0);
        if (*density > cap) *density = static_cast<float>(cap);
    } else if (cfg.atmosphereMode == 1) {
        // GTA density: -log(1-opacity)*1000 / (scale * range_cm); gtaFar = far*fogDistance, start = gtaFar*0.5
        const double opacity = fmin(fmax(cfg.fogOpacity, 0.0), 0.999);
        const double gtaFar = far * cfg.fogDistance;
        const double rangeCm = (gtaFar - gtaFar * 0.5) * 100.0;
        if (rangeCm > 0.0) {
            *density = static_cast<float>(-log(1.0 - opacity) * 1000.0 / (kGtaScale * rangeCm));
        }
    }
}

void Init() {
    g_classicFlag = static_cast<uint8_t*>(Sym(kClassicFlagSym));
    g_colours = static_cast<const uint8_t*>(Sym(kColoursSym));
    if (!g_classicFlag || !g_colours) { SKY_LOG("sky: classic symbols missing, feature inactive"); return; }
    ObservePre(Point::Tick, ScopePre);
    Observe(Point::Tick, ScopePost);
    ObservePre(Point::FogUpdateColors, ScopePre);
    Observe(Point::FogUpdateColors, ScopePost);
    Observe(Point::FogUpdateColors, OnFogUpdateColors);
}

Registrar reg(Feature{"Sky", Init, nullptr});

} // namespace
