// The UE-side part of PC SkyGfxDE's radiosity (core.cpp WriteVolume): the PS2 had no bloom, radiosity was its glow,
// so while [Radiosity] is on DE's BloomIntensity becomes DE's value x DEBloom (UE's 0.675 default when DE leaves it
// unoverridden). DE's value comes back when radiosity is off. The glow itself is drawn on the GPU (vkfx.cpp).
// Feature contract: skygfx.h.
#include <math.h>
#include <stdint.h>
#include "skygfx.h"

namespace skygfx {
namespace {

typedef void* (*StaticFn)();
constexpr float kUeDefaultBloom = 0.675f;

uint32_t g_settings = 0, g_bloom = 0;  // APostProcessVolume::Settings, FPostProcessSettings::BloomIntensity
uint8_t g_ovByte = 0, g_ovMask = 0;    // bOverride_BloomIntensity
bool g_ok = false;

// Per volume: DE's own bloom and override bit, and what we wrote last. DE does not rewrite every field every
// update, so a field still holding our value is ours, not DE's (no compounding).
struct Mem { uint8_t* s; bool active; float written, de; bool deOverride; };
Mem g_mem[8];

Mem* MemFor(uint8_t* s) {
    for (Mem& m : g_mem)
        if (m.s == s) return &m;
    for (Mem& m : g_mem)
        if (!m.s) { m = { s, false, NAN, 0.0f, false }; return &m; }
    return nullptr;
}

void OnPPUpdate(void* self, void*, float, float) {
    if (!g_ok || !self) return;
    uint8_t* s = (uint8_t*)self + g_settings;
    Mem* m = MemFor(s);
    if (!m) return;
    float* bloom = (float*)(s + g_bloom);
    if (!m->active || *bloom != m->written) {  // DE's values (first sight, or DE rewrote them)
        m->de = *bloom;
        m->deOverride = (s[g_ovByte] & g_ovMask) != 0;
    }
    if (cfg.enabled && cfg.radiosity) {
        s[g_ovByte] |= g_ovMask;
        *bloom = (m->deOverride ? m->de : kUeDefaultBloom) * cfg.radiosityDeBloom;
        m->written = *bloom;
        m->active = true;
    } else if (m->active) {
        *bloom = m->de;
        if (m->deOverride) s[g_ovByte] |= g_ovMask;
        else s[g_ovByte] &= (uint8_t)~g_ovMask;
        m->active = false;
    }
}

void Init() {
    StaticFn sstruct = reinterpret_cast<StaticFn>(Sym("_Z12StaticStructI20FPostProcessSettingsEP13UScriptStructv"));
    StaticFn sppv = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI18APostProcessVolumeEP6UClassv"));
    void* settingsStruct = sstruct ? sstruct() : nullptr;
    void* ppvCls = sppv ? sppv() : nullptr;
    void* ov = settingsStruct ? FindProperty(settingsStruct, "bOverride_BloomIntensity") : nullptr;
    g_ok = ppvCls && ov && FieldOffset(ppvCls, "Settings", &g_settings) &&
           FieldOffset(settingsStruct, "BloomIntensity", &g_bloom);
    if (!g_ok) {
        SKY_LOG("postfx: reflection unresolved, DE bloom left alone while radiosity is on");
        return;
    }
    g_ovByte = *((const uint8_t*)ov + UE::Bool_ByteOffset);
    g_ovMask = *((const uint8_t*)ov + UE::Bool_FieldMask);
    Observe(Point::PPUpdateColorOptions, OnPPUpdate);
}

const Feature kFeature = {"postfx", Init, nullptr};
const Registrar kRegistrar(kFeature);

}  // namespace
}  // namespace skygfx
