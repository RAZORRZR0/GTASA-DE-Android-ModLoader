// Modern haze / groundHaze scaling of the height-fog density and the Classic-sky r.VolumetricCloud override for the
// Android UE4.26 build. Ported from SkyGfxDE core.cpp (Hooked_FogUpdateColors haze block, PerFrame cloud block).
#include <stdint.h>
#include "skygfx.h"

using namespace skygfx;

namespace {

const char* const kMarkDirtySym = "_ZN15UActorComponent20MarkRenderStateDirtyEv";
const char* const kFogStructSym = "_Z12StaticStructI25FExponentialHeightFogDataEP13UScriptStructv";
const char* const kCloudCVar = "r.VolumetricCloud";

typedef void (*MarkDirty_Fn)(void* component);
typedef void* (*StaticStruct_Fn)();
MarkDirty_Fn g_markDirty = nullptr;

// Offsets resolved by name from the live classes on first use.
bool g_actorOk = false, g_compOk = false;
uint32_t g_offComp = 0;          // AGTAHeightFog::HeightFogComponent
uint32_t g_offDensity = 0;       // UExponentialHeightFogComponent::FogDensity
uint32_t g_offSecond = 0;        // UExponentialHeightFogComponent::SecondFogData (offset of the struct)
uint32_t g_offSecondDensity = 0; // FExponentialHeightFogData::FogDensity (offset inside the struct)
void* g_fogStruct = nullptr;     // UScriptStruct FExponentialHeightFogData

// DE's values as the original left them, and what we wrote last time. We restore DE's values before the original
// runs so its change detection sees its own value, which keeps the scale from compounding frame to frame.
uint8_t* g_appComp = nullptr;
float g_deMain = 0.0f, g_deSecond = 0.0f;
float g_appMain = 0.0f, g_appSecond = 0.0f;
bool g_applied = false;

int32_t g_deCloud = -1;        // r.VolumetricCloud value saved while the Classic sky overrides it

bool EnsureActorOffsets(const uint8_t* actor) {
    if (g_actorOk) return true;
    void* actorCls = *reinterpret_cast<void* const*>(actor + 0x10); // UObject::ClassPrivate
    g_actorOk = actorCls && FieldOffset(actorCls, "HeightFogComponent", &g_offComp);
    return g_actorOk;
}

bool EnsureCompOffsets(const uint8_t* comp) {
    if (g_compOk) return true;
    void* compCls = *reinterpret_cast<void* const*>(comp + 0x10); // UObject::ClassPrivate
    g_compOk = compCls && g_fogStruct && FieldOffset(compCls, "FogDensity", &g_offDensity) &&
               FieldOffset(compCls, "SecondFogData", &g_offSecond) &&
               FieldOffset(g_fogStruct, "FogDensity", &g_offSecondDensity);
    if (!g_compOk) SKY_LOG("MISSING skyfog offsets (FogDensity / SecondFogData)");
    return g_compOk;
}

// Pre-FogUpdateColors: put DE's values back if the component still holds what we wrote last frame.
void PreFog(void* self, void*, float, float) {
    if (!self || !OnGameThread() || !g_applied) return;
    const uint8_t* actor = static_cast<const uint8_t*>(self);
    if (!EnsureActorOffsets(actor)) return;
    uint8_t* comp = *reinterpret_cast<uint8_t* const*>(actor + g_offComp);
    if (comp && comp == g_appComp && g_compOk) {
        float* d = reinterpret_cast<float*>(comp + g_offDensity);
        float* s = reinterpret_cast<float*>(comp + g_offSecond + g_offSecondDensity);
        if (*d == g_appMain && *s == g_appSecond) {
            *d = g_deMain;
            *s = g_deSecond;
        }
    }
    g_applied = false;
}

// Post-FogUpdateColors: scale DE's fresh values for modern fog; r.VolumetricCloud off while the Classic sky is on.
void PostFog(void* self, void*, float, float) {
    if (!self || !OnGameThread()) return;
    const uint8_t* actor = static_cast<const uint8_t*>(self);
    if (!EnsureActorOffsets(actor)) return;
    uint8_t* comp = *reinterpret_cast<uint8_t* const*>(actor + g_offComp);
    if (comp && EnsureCompOffsets(comp)) {
        float* d = reinterpret_cast<float*>(comp + g_offDensity);
        float* s = reinterpret_cast<float*>(comp + g_offSecond + g_offSecondDensity);
        g_deMain = *d;
        g_deSecond = *s;
        // shortcut: GroundHaze is applied only in modern mode; PC also applies it under GTA fog.
        const bool modern = cfg.enabled && cfg.atmosphereMode == 2;
        const float wantMain = modern ? g_deMain * cfg.haze : g_deMain;
        const float wantSecond = modern ? g_deSecond * cfg.groundHaze : g_deSecond;
        if (modern && (wantMain != *d || wantSecond != *s)) {
            *d = wantMain;
            *s = wantSecond;
            if (g_markDirty) g_markDirty(comp);
            g_appComp = comp;
            g_appMain = wantMain;
            g_appSecond = wantSecond;
            g_applied = true;
        }
    }

    const bool classic = cfg.enabled && cfg.atmosphereMode == 0;
    if (classic) {
        if (g_deCloud < 0) g_deCloud = GetCVarInt(kCloudCVar, -1);
        if (g_deCloud >= 0) SetCVarInt(kCloudCVar, 0);
    } else if (g_deCloud >= 0) {
        SetCVarInt(kCloudCVar, g_deCloud);
        g_deCloud = -1;
    }
}

void Init() {
    g_markDirty = reinterpret_cast<MarkDirty_Fn>(Sym(kMarkDirtySym));
    auto staticStruct = reinterpret_cast<StaticStruct_Fn>(Sym(kFogStructSym));
    if (!g_markDirty || !staticStruct) { SKY_LOG("skyfog: symbols missing, haze disabled"); return; }
    g_fogStruct = staticStruct();
    ObservePre(Point::FogUpdateColors, PreFog);
    Observe(Point::FogUpdateColors, PostFog);
}

Registrar reg(Feature{"SkyFog", Init, nullptr});

} // namespace
