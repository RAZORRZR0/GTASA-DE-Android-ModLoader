// Street lights, ported from PC streetlights.cpp.
// Distance part: writes the int32 globals GStreetLightDistance and GStreetlightShadowDistance in cm (ini is decametres, Dm * 1000).
// Shadow part (OtherLightShadowsFrame): with cfg.otherLightShadows, point and spot lights cast shadows except on skip-listed
// owners. Lamp meshes lose bNeverCastShadows, and fixture meshes smaller than kFixtureRadius stop casting shadows.
// Bollard and globe lamp lights get intensity multipliers.
// DE values are kept and restored when a part is off. Properties are found by name; setters are called as native exports.
// Game thread only for UObject access. configChanged only sets state for Tick.
#include <mod/amlmod.h>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include "skygfx.h"

using namespace skygfx;

namespace {

int32_t* g_drawDist = nullptr;   // GStreetLightDistance
int32_t* g_shadowDist = nullptr; // GStreetlightShadowDistance
int32_t g_origDraw = 0, g_origShadow = 0; // DE values, restored when the feature is off
bool g_haveOrig = false;
bool g_ok = false;

// Applies the wanted values to the live globals. Runs every Tick, so a game that rewrites them is corrected.
void ApplyDistances() {
    if (!cfg.enabled) {
        if (g_haveOrig) { *g_drawDist = g_origDraw; *g_shadowDist = g_origShadow; }
        return;
    }
    if (!g_haveOrig) { g_origDraw = *g_drawDist; g_origShadow = *g_shadowDist; g_haveOrig = true; }
    const int32_t wantDraw = cfg.streetLightDrawDm * 1000, wantShadow = cfg.streetLightShadowDm * 1000;
    if (*g_drawDist != wantDraw) *g_drawDist = wantDraw;
    if (*g_shadowDist != wantShadow) *g_shadowDist = wantShadow;
}

void LightsTick(void*, void*, float, float) {
    if (g_ok && skygfx::OnGameThread()) ApplyDistances();
}

// ---- Other light shadows and bollard/globe brightness

typedef void* (*StaticClassFn)();
typedef void (*SetCastShadowsFn)(void* self, bool on);
typedef void (*SetIntensityFn)(void* self, float value);
typedef void (*FNameCtorFn)(void* self, const char* name, int findType);

SetCastShadowsFn fnLightCast = nullptr;   // ULightComponentBase::SetCastShadows
SetCastShadowsFn fnPrimCast = nullptr;    // UPrimitiveComponent::SetCastShadow
SetIntensityFn fnSetIntensity = nullptr;  // ULightComponent::SetIntensity
FNameCtorFn fnFName = nullptr;            // FName(const char*, EFindName)

uint8_t* g_objects = nullptr;  // GUObjectArray + 0x10: chunks at +0, count at +0x14

constexpr size_t kObjFlags = 0x08, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kSuperStruct = 0x40;           // UStruct::SuperStruct
constexpr size_t kSphereRadiusInBounds = 0x18;  // FBoxSphereBounds::SphereRadius
constexpr uint32_t kRfClassDefault = 0x10, kRfArchetype = 0x20;
constexpr uint32_t kItemUnreachable = 1u << 28, kItemPendingKill = 1u << 29;
constexpr int kCallBudget = 32, kSlotsPerFrame = 4096;
constexpr float kFixtureRadius = 600.0f;

// A class or owner name and its FName index. -1 = not in the FName pool yet (matches nothing).
struct NameSlot { const char* name; int32_t index; };
NameSlot g_point{"PointLightComponent", -1}, g_spot{"SpotLightComponent", -1}, g_prim{"PrimitiveComponent", -1};
NameSlot g_street{"StreetLightMapActor", -1};
NameSlot g_bollard{"BP_Streetlight_BollardPark_C", -1};
NameSlot g_globe[] = { {"BP_Streetlamp1_C", -1}, {"BP_Streetlamp2_C", -1} };
NameSlot g_skip[] = {
    {"BP_Vehicle_Base_C", -1}, {"BP_Vehicle_Cinematic_Base_C", -1}, {"BP_GTASA_UpgradePart_C", -1},
    {"BP_VehicleLight_Blinking_C", -1}, {"BP_VehicleLight_Dash_C", -1}, {"BP_VehicleLight_Dome_C", -1},
    {"BP_HelicopterSpotLight_C", -1}, {"BP_RotatingColoredSpotLight_C", -1}, {"BP_Pickup_Base_C", -1},
    {"BP_Pickup_Camera_C", -1}, {"BP_Explosion_Base_C", -1}, {"BP_SmokeFireLick_C", -1},
    {"BP_Marker_Cylinder_C", -1}, {"BP_RailTriggerLight_SnailTrail_C", -1}, {"BP_Singleton_C", -1},
    {"BP_SanAndreasInterface_C", -1},
};

// Bool bitfield property: the byte is at FieldOffset + ByteOffset, selected by FieldMask.
struct BitRef { size_t at = 0; uint8_t mask = 0; };
bool BitTest(const uint8_t* o, BitRef b) { return (o[b.at] & b.mask) != 0; }
void BitPut(uint8_t* o, BitRef b, bool on) { if (on) o[b.at] |= b.mask; else o[b.at] &= (uint8_t)~b.mask; }

struct Schema {
    BitRef lightCast, lightDyn, primCast, lampNever;
    size_t intensity = 0;     // ULightComponentBase::Intensity (float)
    size_t sphereRadius = 0;  // USceneComponent::Bounds.SphereRadius
    bool ok = false;
} g_s;

enum LampKind { kPlain = 0, kBollard = 1, kGlobe = 2 };

struct Light {
    uint8_t* comp;
    uint8_t* owner;
    float intensity;  // DE value, restored when the light goes back to plain
    LampKind kind;
    bool neverWas;    // owner had bNeverCastShadows, and we cleared it
    bool castWasOff;  // CastShadows was off in DE
    bool dynWasOff;   // CastDynamicShadows was off in DE
};

std::unordered_map<int32_t, Light> g_lights;     // by GUObjectArray slot
std::unordered_map<int32_t, uint8_t*> g_meshes;  // fixture mesh slot -> object whose CastShadow we turned off
std::unordered_set<uint8_t*> g_owners;           // owners of tracked lights; the mesh pass only looks at these
int32_t g_cursor = 0;
bool g_appliedShadows = false;
float g_applied[3] = {1.0f, 1.0f, 1.0f};  // scale last applied per kind

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

void SetLightCast(uint8_t* comp, bool on) { fnLightCast(comp, on); }

// Turning shadows on or off re-registers the light with the renderer: the setter must be called twice.
void ReRegister(uint8_t* comp, bool first, bool second) {
    SetLightCast(comp, first);
    SetLightCast(comp, second);
}

void RestoreLight(const Light& l) {
    if (l.dynWasOff) BitPut(l.comp, g_s.lightDyn, false);
    if (l.castWasOff || l.dynWasOff) ReRegister(l.comp, l.castWasOff, !l.castWasOff);
    if (l.neverWas) BitPut(l.owner, g_s.lampNever, true);
    if (l.kind != kPlain) fnSetIntensity(l.comp, l.intensity);
}

void RestoreMesh(int32_t slot, uint8_t* obj) {
    if (Live(slot, obj)) fnPrimCast(obj, true);
}

void Prune() {
    for (auto it = g_lights.begin(); it != g_lights.end();)
        it = Live(it->first, it->second.comp) ? std::next(it) : g_lights.erase(it);
    for (auto it = g_meshes.begin(); it != g_meshes.end();)
        it = Live(it->first, it->second) ? std::next(it) : g_meshes.erase(it);
    g_owners.clear();
    for (auto& kv : g_lights) g_owners.insert(kv.second.owner);
}

// Restores every tracked light and mesh, and forgets them. A changed setting rescans from scratch.
void RestoreAll() {
    for (auto& kv : g_lights)
        if (Live(kv.first, kv.second.comp)) RestoreLight(kv.second);
    for (auto& kv : g_meshes) RestoreMesh(kv.first, kv.second);
    g_lights.clear();
    g_meshes.clear();
    g_owners.clear();
}

// Runs once per Tick. Scans up to kSlotsPerFrame GObjects slots and makes at most kCallBudget setter calls.
void OtherLightShadowsFrame() {
    const bool shadows = cfg.enabled && cfg.otherLightShadows;
    const float scale[3] = { 1.0f, ScaleOf(kBollard), ScaleOf(kGlobe) };
    const bool unscaled = scale[kBollard] == 1.0f && scale[kGlobe] == 1.0f;
    if (shadows != g_appliedShadows || (!shadows && unscaled)) {
        RestoreAll();
        g_appliedShadows = shadows;
        g_applied[kBollard] = g_applied[kGlobe] = 1.0f;
        g_cursor = 0;
        if (!shadows && unscaled) return;
    }
    if (scale[kBollard] != g_applied[kBollard] || scale[kGlobe] != g_applied[kGlobe]) {
        for (auto& kv : g_lights) {
            const Light& l = kv.second;
            if (l.kind != kPlain && Live(kv.first, l.comp)) fnSetIntensity(l.comp, l.intensity * scale[l.kind]);
        }
        g_applied[kBollard] = scale[kBollard];
        g_applied[kGlobe] = scale[kGlobe];
    }

    ResolveSlots(&g_point, 1);
    ResolveSlots(&g_spot, 1);
    ResolveSlots(&g_prim, 1);
    if (g_point.index < 0 || g_spot.index < 0 || g_prim.index < 0) return;

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
        uint8_t* owner = *(uint8_t**)(obj + kObjOuter);
        if (!owner) continue;

        const int32_t cls = NameOf(klass);
        const bool light = cls == g_point.index || cls == g_spot.index;
        if (!light) {
            // Fixture meshes: small ones under a tracked light stop casting, so they don't black out that light.
            if (!shadows || !g_owners.count(owner)) continue;
            auto m = g_meshes.find(slot);
            if (m != g_meshes.end() && m->second == obj) continue;
            if (ChainMatches(obj, &g_prim, 1) && BitTest(obj, g_s.primCast) &&
                *(const float*)(obj + g_s.sphereRadius) < kFixtureRadius) {
                fnPrimCast(obj, false);
                g_meshes[slot] = obj;
                ++calls;
            }
            continue;
        }

        auto seen = g_lights.find(slot);
        if (seen != g_lights.end()) {
            if (seen->second.comp == obj) continue;
            g_lights.erase(seen);
        }
        if (ChainMatches(owner, g_skip, sizeof g_skip / sizeof g_skip[0])) continue;

        const LampKind kind = KindOf(owner);
        const bool street = ChainMatches(owner, &g_street, 1);
        Light l{obj, owner, *(const float*)(obj + g_s.intensity), kind,
                street && BitTest(owner, g_s.lampNever), !BitTest(obj, g_s.lightCast), !BitTest(obj, g_s.lightDyn)};
        if (shadows) {
            if (l.neverWas) BitPut(owner, g_s.lampNever, false);
            if (l.castWasOff || l.dynWasOff) {
                BitPut(obj, g_s.lightDyn, true);
                ReRegister(obj, false, true);
                ++calls;
            }
            g_owners.insert(owner);
        } else {
            l.neverWas = l.castWasOff = l.dynWasOff = false;  // nothing changed, nothing to restore
        }
        if (kind != kPlain && scale[kind] != 1.0f) {
            fnSetIntensity(obj, l.intensity * scale[kind]);
            ++calls;
        }
        g_lights.emplace(slot, l);
    }
}

void ShadowTick(void*, void*, float, float) {
    if (g_s.ok && skygfx::OnGameThread()) OtherLightShadowsFrame();
}

// Resolves a native class by its StaticClass export.
void* StaticClassOf(const char* sym) {
    auto f = (StaticClassFn)Sym(sym);
    return f ? f() : nullptr;
}

// Bool bitfield property by name: byte at FieldOffset + ByteOffset, bit FieldMask.
bool BitOf(void* ustruct, const char* name, BitRef& out) {
    uint32_t off = 0;
    void* p = FindProperty(ustruct, name);
    if (!p || !FieldOffset(ustruct, name, &off)) return false;
    out.at = off + *((const uint8_t*)p + UE::Bool_ByteOffset);
    out.mask = *((const uint8_t*)p + UE::Bool_FieldMask);
    return true;
}

void InitOtherLights() {
    uint8_t* gua = (uint8_t*)Sym("GUObjectArray");
    fnFName = (FNameCtorFn)Sym("_ZN5FNameC1EPKc9EFindName");
    fnLightCast = (SetCastShadowsFn)Sym("_ZN19ULightComponentBase14SetCastShadowsEb");
    fnPrimCast = (SetCastShadowsFn)Sym("_ZN19UPrimitiveComponent13SetCastShadowEb");
    fnSetIntensity = (SetIntensityFn)Sym("_ZN15ULightComponent12SetIntensityEf");
    void* baseC = StaticClassOf("_ZN19ULightComponentBase11StaticClassEv");
    void* primC = StaticClassOf("_ZN19UPrimitiveComponent11StaticClassEv");
    void* sceneC = StaticClassOf("_ZN15USceneComponent11StaticClassEv");
    void* lampC = StaticClassOf("_ZN20AStreetLightMapActor11StaticClassEv");
    uint32_t intensity = 0, bounds = 0;
    const bool syms = gua && fnFName && fnLightCast && fnPrimCast && fnSetIntensity;
    const bool cls = baseC && primC && sceneC && lampC;
    const bool b1 = cls && BitOf(baseC, "CastShadows", g_s.lightCast), b2 = cls && BitOf(baseC, "CastDynamicShadows", g_s.lightDyn);
    const bool b3 = cls && BitOf(primC, "CastShadow", g_s.primCast), b4 = cls && BitOf(lampC, "bNeverCastShadows", g_s.lampNever);
    const bool f1 = cls && FieldOffset(baseC, "Intensity", &intensity);
    // Bounds is a native (unreflected) member: USceneComponent::UpdateBounds (0x8c1287c) stores it at this+0x100.
    const bool f2 = cls && (FieldOffset(sceneC, "Bounds", &bounds) || (bounds = 0x100, true));
    if (!(syms && cls && b1 && b2 && b3 && b4 && f1 && f2)) {
        SKY_LOG("lights: other light shadows and bollard/globe brightness off (syms %d classes %d/%d/%d/%d "
                "CastShadows %d CastDynamicShadows %d CastShadow %d bNeverCastShadows %d Intensity %d Bounds %d)",
                syms, !!baseC, !!primC, !!sceneC, !!lampC, b1, b2, b3, b4, f1, f2);
        return;
    }
    g_objects = gua + 0x10;
    g_s.intensity = intensity;
    g_s.sphereRadius = bounds + kSphereRadiusInBounds;
    g_s.ok = true;
    Observe(Point::Tick, &ShadowTick);
}

void Init() {
    g_drawDist = (int32_t*)Sym("GStreetLightDistance");
    g_shadowDist = (int32_t*)Sym("GStreetlightShadowDistance");
    if (!g_drawDist || !g_shadowDist) {
        SKY_LOG("lights: distance globals missing, street light distance off");
    } else {
        g_ok = true;
        Observe(Point::Tick, &LightsTick);
    }
    InitOtherLights();
}

void ConfigChanged() {}

const Feature kFeature = { "StreetLights", Init, ConfigChanged };
Registrar g_registrar(kFeature);

}  // namespace
