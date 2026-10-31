// Street lights, ported from PC streetlights.cpp.
// Distance: writes the int32 global GStreetLightDistance in cm (ini is decametres, Dm * 1000).
// Brightness: park bollard and globe lamp lights get intensity multipliers (PC BollardBrightness / GlobeLampBrightness).
// PC's OtherLightShadows and lamp shadow distance are not ported: DE Android's mobile renderer was built without movable
// spot/point light shadows (r.Mobile.EnableMovableSpotlightsShadow 0; forcing it at load, with every lamp casting and DE's
// streetlight shadow switch on, still draws no shadow), so lamp shadows cannot exist on Android.
// DE values are kept and restored when a part is off. Properties are found by name; setters are called as native exports.
// Game thread only for UObject access. configChanged only sets state for Tick.
#include <mod/amlmod.h>
#include <iterator>
#include <unordered_map>
#include "skygfx.h"

using namespace skygfx;

namespace {

int32_t* g_drawDist = nullptr;  // GStreetLightDistance
int32_t g_origDraw = 0;         // DE value, restored when the feature is off
bool g_haveOrig = false;
bool g_ok = false;

// Applies the wanted value to the live global. Runs every Tick, so a game that rewrites it is corrected.
void ApplyDistances() {
    if (!cfg.enabled) {
        if (g_haveOrig) *g_drawDist = g_origDraw;
        return;
    }
    if (!g_haveOrig) { g_origDraw = *g_drawDist; g_haveOrig = true; }
    const int32_t wantDraw = cfg.streetLightDrawDm * 1000;
    if (*g_drawDist != wantDraw) *g_drawDist = wantDraw;
}

void LightsTick(void*, void*, float, float) {
    if (g_ok && skygfx::OnGameThread()) ApplyDistances();
}

// ---- Bollard/globe brightness

typedef void* (*StaticClassFn)();
typedef void (*SetIntensityFn)(void* self, float value);
typedef void (*FNameCtorFn)(void* self, const char* name, int findType);

SetIntensityFn fnSetIntensity = nullptr;  // ULightComponent::SetIntensity
FNameCtorFn fnFName = nullptr;            // FName(const char*, EFindName)

uint8_t* g_objects = nullptr;  // GUObjectArray + 0x10: chunks at +0, count at +0x14

constexpr size_t kObjFlags = 0x08, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kSuperStruct = 0x40;  // UStruct::SuperStruct
constexpr uint32_t kRfClassDefault = 0x10, kRfArchetype = 0x20;
constexpr uint32_t kItemUnreachable = 1u << 28, kItemPendingKill = 1u << 29;
constexpr int kCallBudget = 32, kSlotsPerFrame = 4096;

// A class or owner name and its FName index. -1 = not in the FName pool yet (matches nothing).
struct NameSlot { const char* name; int32_t index; };
NameSlot g_point{"PointLightComponent", -1}, g_spot{"SpotLightComponent", -1};
NameSlot g_street{"StreetLightMapActor", -1};
NameSlot g_bollard{"BP_Streetlight_BollardPark_C", -1};
NameSlot g_globe[] = { {"BP_Streetlamp1_C", -1}, {"BP_Streetlamp2_C", -1} };

size_t g_intensityOff = 0;  // ULightComponentBase::Intensity (float)
bool g_lightsOk = false;

enum LampKind { kPlain = 0, kBollard = 1, kGlobe = 2 };

struct Light {
    uint8_t* comp;
    float intensity;  // DE value, restored when the light goes back to plain
    LampKind kind;
};

std::unordered_map<int32_t, Light> g_lights;  // bollard and globe lights, by GUObjectArray slot
int32_t g_cursor = 0;
float g_applied[3] = {1.0f, 1.0f, 1.0f};  // scale last applied per kind
uint8_t* g_sampleObj = nullptr;  // one street lamp light, for Status
int32_t g_sampleSlot = -1;
uint32_t g_maxDrawOff = 0;       // ULightComponent::MaxDrawDistance (reflected), 0 = unknown

uint8_t* ObjectItem(int32_t i) {
    uint8_t** chunks = *reinterpret_cast<uint8_t***>(g_objects);
    uint8_t* chunk = chunks ? chunks[i >> 16] : nullptr;
    return chunk ? chunk + (size_t)(i & 0xFFFF) * 0x18 : nullptr;
}

uint8_t* ClassOf(const uint8_t* obj) { return *(uint8_t* const*)(obj + kObjClass); }
int32_t NameOf(const uint8_t* obj) { return *(const int32_t*)(obj + kObjName); }

// Lookup only (FNAME_Find): returns -1 when the engine never interned the name.
int32_t FindName(const char* s) {
    int32_t raw[2] = {0, 0};
    fnFName(raw, s, 0);
    return raw[0] > 0 ? raw[0] : -1;
}

void ResolveSlots(NameSlot* s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i].index < 0) s[i].index = FindName(s[i].name);
}

// True when obj's class chain contains a class whose name is one of the slots.
bool ChainMatches(const uint8_t* obj, NameSlot* s, size_t n) {
    ResolveSlots(s, n);
    for (const uint8_t* c = ClassOf(obj); c; c = *(uint8_t* const*)(c + kSuperStruct)) {
        const int32_t nm = NameOf(c);
        for (size_t i = 0; i < n; ++i)
            if (s[i].index >= 0 && s[i].index == nm) return true;
    }
    return false;
}

// The slot still holds this object, and it is not unreachable or pending kill.
bool Live(int32_t slot, const uint8_t* obj) {
    uint8_t* item = ObjectItem(slot);
    return item && *(uint8_t* const*)item == obj && !(*(const uint32_t*)(item + 8) & (kItemUnreachable | kItemPendingKill));
}

LampKind KindOf(const uint8_t* owner) {
    if (ChainMatches(owner, &g_bollard, 1)) return kBollard;
    if (ChainMatches(owner, g_globe, 2)) return kGlobe;
    return kPlain;
}

float ScaleOf(LampKind k) {
    if (!cfg.enabled) return 1.0f;
    if (k == kBollard) return cfg.bollardBrightness;
    if (k == kGlobe) return (float)cfg.globeLampBrightness;
    return 1.0f;
}

void Prune() {
    for (auto it = g_lights.begin(); it != g_lights.end();)
        it = Live(it->first, it->second.comp) ? std::next(it) : g_lights.erase(it);
}

// Restores every tracked light's DE intensity, and forgets them. A changed setting rescans from scratch.
void RestoreAll() {
    for (auto& kv : g_lights)
        if (Live(kv.first, kv.second.comp)) fnSetIntensity(kv.second.comp, kv.second.intensity);
    g_lights.clear();
}

// Runs once per Tick. Scans up to kSlotsPerFrame GObjects slots and makes at most kCallBudget setter calls.
void BrightnessFrame() {
    const float scale[3] = { 1.0f, ScaleOf(kBollard), ScaleOf(kGlobe) };
    if (scale[kBollard] == 1.0f && scale[kGlobe] == 1.0f) {
        if (!g_lights.empty()) RestoreAll();
        g_applied[kBollard] = g_applied[kGlobe] = 1.0f;
        g_cursor = 0;
        return;
    }
    if (scale[kBollard] != g_applied[kBollard] || scale[kGlobe] != g_applied[kGlobe]) {
        for (auto& kv : g_lights) {
            const Light& l = kv.second;
            if (Live(kv.first, l.comp)) fnSetIntensity(l.comp, l.intensity * scale[l.kind]);
        }
        g_applied[kBollard] = scale[kBollard];
        g_applied[kGlobe] = scale[kGlobe];
    }

    ResolveSlots(&g_point, 1);
    ResolveSlots(&g_spot, 1);
    if (g_point.index < 0 || g_spot.index < 0) return;

    const int32_t count = *(const int32_t*)(g_objects + 0x14);
    if (count <= 0) return;

    int calls = 0;
    for (int n = 0; n < kSlotsPerFrame && calls < kCallBudget; ++n) {
        if (g_cursor >= count) { g_cursor = 0; Prune(); }  // full pass done: drop dead entries
        const int32_t slot = g_cursor++;
        uint8_t* item = ObjectItem(slot);
        uint8_t* obj = item ? *(uint8_t**)item : nullptr;
        if (!obj || (*(const uint32_t*)(item + 8) & (kItemUnreachable | kItemPendingKill))) continue;
        if (*(const uint32_t*)(obj + kObjFlags) & (kRfClassDefault | kRfArchetype)) continue;
        uint8_t* klass = ClassOf(obj);
        if (!klass) continue;
        const int32_t cls = NameOf(klass);
        if (cls != g_point.index && cls != g_spot.index) continue;
        uint8_t* owner = *(uint8_t**)(obj + kObjOuter);
        if (!owner) continue;

        auto seen = g_lights.find(slot);
        if (seen != g_lights.end()) {
            if (seen->second.comp == obj) continue;
            g_lights.erase(seen);  // slot reused by a new light
        }
        if (!g_sampleObj && ChainMatches(owner, &g_street, 1)) { g_sampleObj = obj; g_sampleSlot = slot; }
        const LampKind kind = KindOf(owner);
        if (kind == kPlain) continue;
        const Light l{obj, *(const float*)(obj + g_intensityOff), kind};
        if (scale[kind] != 1.0f) {
            fnSetIntensity(obj, l.intensity * scale[kind]);
            ++calls;
        }
        g_lights.emplace(slot, l);
    }
}

void BrightnessTick(void*, void*, float, float) {
    if (g_lightsOk && skygfx::OnGameThread()) BrightnessFrame();
}

// Resolves a native class by its StaticClass export.
void* StaticClassOf(const char* sym) {
    auto f = (StaticClassFn)Sym(sym);
    return f ? f() : nullptr;
}

void InitBrightness() {
    uint8_t* gua = (uint8_t*)Sym("GUObjectArray");
    fnFName = (FNameCtorFn)Sym("_ZN5FNameC1EPKc9EFindName");
    fnSetIntensity = (SetIntensityFn)Sym("_ZN15ULightComponent12SetIntensityEf");
    void* baseC = StaticClassOf("_ZN19ULightComponentBase11StaticClassEv");
    uint32_t intensity = 0;
    if (!gua || !fnFName || !fnSetIntensity || !baseC || !FieldOffset(baseC, "Intensity", &intensity)) {
        SKY_LOG("lights: bollard/globe brightness off (symbols or Intensity unresolved)");
        return;
    }
    g_objects = gua + 0x10;
    g_intensityOff = intensity;
    g_lightsOk = true;
    if (void* lightC = StaticClassOf("_ZN15ULightComponent11StaticClassEv")) FieldOffset(lightC, "MaxDrawDistance", &g_maxDrawOff);
    Observe(Point::Tick, &BrightnessTick);
}

void Init() {
    g_drawDist = (int32_t*)Sym("GStreetLightDistance");
    if (!g_drawDist) {
        SKY_LOG("lights: distance global missing, street light distance off");
    } else {
        g_ok = true;
        Observe(Point::Tick, &LightsTick);
    }
    InitBrightness();
}

void ConfigChanged() {}

void Status() {
    int kinds[3] = {};
    for (auto& kv : g_lights) ++kinds[kv.second.kind];
    if (g_sampleObj && !Live(g_sampleSlot, g_sampleObj)) g_sampleObj = nullptr;
    const uint8_t* s = g_sampleObj;
    SKY_LOG("lights: draw=%d (DE %d) bollard=%d globe=%d sample=%p intensity=%.0f maxdraw=%.0f",
            g_drawDist ? *g_drawDist : -1, g_origDraw, kinds[kBollard], kinds[kGlobe], s,
            s ? *(const float*)(s + g_intensityOff) : -1.0f, s && g_maxDrawOff ? *(const float*)(s + g_maxDrawOff) : -1.0f);
}

const Feature kFeature = { "StreetLights", Init, ConfigChanged, false, Status };
Registrar g_registrar(kFeature);

}  // namespace
