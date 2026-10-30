// Distant lamp coronas, ported from PC coronas.cpp (Project2DFX SALodLights).
// SALodLights.dat lists corona offsets per model. Every IPL instance of those models that CFileLoader::
// LoadObjectInstance builds during level load becomes a lamp (instance position + offset turned by the heading).
// Each frame, lamps past the model's own corona range and inside the far clip get a corona in DE's
// UGTACoronaComponent (AGTAWorldSettings::GetCoronaComponent(dynamic)), driven by its exported natives.
// Game thread only.
#include <mod/amlmod.h>
#include "skygfx.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>
using namespace skygfx;
namespace {

// Native-class layouts from the PC build (CBaseModelInfo, CFileObjectInstance). They are not reflectable, so
// they are unverified on Android. Confirm on device before trusting distant lamps.
namespace MDL { constexpr size_t Key = 0x08, DrawDistance = 0x38; }
namespace INST { constexpr size_t Model = 0x1C, Area = 0x20; }
constexpr uint32_t kRfClassDefault = 0x10, kRfArchetype = 0x20;
constexpr float kDefaultFarClip = 1500.0f; // shortcut: PC used the live camera far clip, unreadable here, upgrade by reading TheCamera far clip

struct Light { float off[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; };
struct Lamp { float pos[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; int32_t corona; float scale, intensity; };

std::unordered_map<uint32_t, std::vector<Light>> g_dat; // by model key, until the level has loaded
std::vector<Lamp> g_lamps;
volatile bool g_catching = false;     // LoadObjectInstance adds lamps until the first game frame
uint8_t** g_modelInfos = nullptr;     // CModelInfo::ms_modelInfoPtrs
const uint8_t* g_objects = nullptr;   // GUObjectArray + 0x10 (FChunkedFixedUObjectArray)
const uint8_t* g_hours = nullptr;     // CClock::ms_nGameClockHours
const uint8_t* g_minutes = nullptr;   // CClock::ms_nGameClockMinutes
const uint32_t* g_timeMs = nullptr;   // CTimer::m_snTimeInMilliseconds
const uint8_t* g_currArea = nullptr;  // CGame::currArea (low byte)
const float* g_camMatrix = nullptr;   // TheCamera + 0x18 (unverified), position = row 3
bool g_ok = false;
uint8_t* g_settings = nullptr;        // AGTAWorldSettings of the running world
int32_t g_settingsIndex = -1;
uint8_t* g_component = nullptr;       // its dynamic UGTACoronaComponent
int g_shown = 0;

// Exported natives, called directly (game thread only). FVector is passed as three floats (HFA).
typedef void* (*GetComponent_Fn)(void* settings, bool dynamic, bool upClose);
typedef int32_t (*Add_Fn)(void* comp, float x, float y, float z, float intensity, float scale, uint32_t color);
typedef void (*Update_Fn)(void* comp, int32_t index, const float* pos, float scale, uint32_t color, float intensity);
typedef void (*Remove_Fn)(void* comp, int32_t index);
GetComponent_Fn fnGetComponent = nullptr;
Add_Fn fnAdd = nullptr;
Update_Fn fnUpdate = nullptr;
Remove_Fn fnRemove = nullptr;
typedef void (*FNameCtor_Fn)(void* self, const char* s, int findType);
FNameCtor_Fn fnFNameCtor = nullptr;

// CKeyGen::GetUppercaseKey: CRC-32 table, seed ~0, no final xor, upper-cased. Equals the CBaseModelInfo key.
uint32_t UppercaseKey(const char* s) {
    uint32_t k = 0xFFFFFFFF;
    for (; *s; ++s) {
        uint32_t c = (k ^ (uint32_t)toupper((uint8_t)*s)) & 0xFF;
        for (int i = 0; i < 8; ++i) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        k = c ^ (k >> 8);
    }
    return k;
}

// SALodLights.dat: "%model" sections, then "R G B A offX offY offZ CustomSize DrawDistance Blink NoDistance
// Searchlight" (legacy lines lack DrawDistance). "%additional_coronas" offsets are world positions.
int LoadDat(const char* dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/SALodLights.dat", dir);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    std::vector<Light>* section = nullptr;
    bool world = false;
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line + strspn(line, " \t");
        if (*p == '%') {
            char name[64] = "";
            sscanf(p + 1, "%63s", name);
            world = strcmp(name, "additional_coronas") == 0;
            section = world ? nullptr : &g_dat[UppercaseKey(name)];
            continue;
        }
        Light l{};
        unsigned r, g, b, a, search;
        const int got = sscanf(p, "%u %u %u %u %f %f %f %f %f %d %d %u", &r, &g, &b, &a, &l.off[0], &l.off[1], &l.off[2],
                               &l.size, &l.drawDist, &l.blink, &l.noDist, &search);
        if (got == 11) { l.noDist = l.blink; l.blink = (int)l.drawDist; l.drawDist = 0.0f; }
        else if (got != 12) continue; // comment, blank
        // shortcut: traffic lights (CustomSize 0.45) skipped, upgrade with CTrafficLights phases and per-light road axis.
        if (l.size == 0.45f) continue;
        l.r = (uint8_t)r; l.g = (uint8_t)g; l.b = (uint8_t)b; l.a = (uint8_t)a;
        if (world) g_lamps.push_back({ { l.off[0], l.off[1], l.off[2] }, l.r, l.g, l.b, l.a, l.size, 0.0f, l.blink, l.noDist, -1, 0, 0 });
        else if (section) section->push_back(l);
        ++n;
    }
    fclose(f);
    return n;
}

// One IPL instance: lamps for every dat light of its model. Heading as LoadObjectInstance sets it for an
// untilted instance: acos(qw) x (qz < 0 ? 2 : -2).
void Catch(const uint8_t* inst) {
    const uint8_t* mi = g_modelInfos[*(const uint16_t*)(inst + INST::Model)];
    if (!mi || inst[INST::Area] != 0) return; // interiors never show outside
    auto it = g_dat.find(*(const uint32_t*)(mi + MDL::Key));
    if (it == g_dat.end()) return;
    const float* q = (const float*)inst;
    if (q[3] == 0.0f && q[4] == 0.0f) return; // quaternion x,y both zero: no placement
    const float h = acosf(fminf(fmaxf(q[6], -1.0f), 1.0f)) * (q[5] < 0.0f ? 2.0f : -2.0f);
    const float c = cosf(h), s = sinf(h), dd = *(const float*)(mi + MDL::DrawDistance);
    for (const Light& l : it->second)
        g_lamps.push_back({ { q[0] + c * l.off[0] - s * l.off[1], q[1] + s * l.off[0] + c * l.off[1], q[2] + l.off[2] },
                            l.r, l.g, l.b, l.a, l.size, fminf(l.drawDist, dd), l.blink, l.noDist, -1, 0, 0 });
}

typedef uint8_t* (*LoadInstance_Fn)(const uint8_t* inst, const char* name, uint8_t fromIpl);
LoadInstance_Fn o_LoadInstance = nullptr;
uint8_t* Hooked_LoadInstance(const uint8_t* inst, const char* name, uint8_t fromIpl) {
    uint8_t* e = o_LoadInstance(inst, name, fromIpl);
    if (e && g_catching) Catch(inst);
    return e;
}

// ---------------------------------------------------------------- UE side

// FUObjectItem: 0x18 stride, chunks of 65536 (verified by propfix).
uint8_t* ObjectItem(int32_t index) {
    if (!g_objects || index < 0 || index >= *(const int32_t*)(g_objects + 0x14)) return nullptr;
    uint8_t* const* chunks = *(uint8_t* const* const*)g_objects;
    return chunks[index >> 16] + (index & 0xFFFF) * 0x18;
}

// UObject: Flags +0x08, ClassPrivate +0x10, NamePrivate +0x18, OuterPrivate +0x20 (UE 4.26).
int32_t NameOf(const uint8_t* obj) { return *(const int32_t*)(obj + 0x18); }
const uint8_t* ClassOf(const uint8_t* obj) { return *(const uint8_t* const*)(obj + 0x10); }

int32_t FindName(const char* s) {
    struct { int32_t index, number; } n = { 0, 0 };
    fnFNameCtor(&n, s, 0 /* FNAME_Find */);
    return n.index > 0 ? n.index : -1;
}

bool Live() {
    uint8_t* item = ObjectItem(g_settingsIndex);
    return item && *(uint8_t**)item == g_settings;
}

// GObjects pass: the AGTAWorldSettings instance whose corona component DE's lights already use.
bool Resolve() {
    const int32_t nSettings = FindName("GTAWorldSettings");
    if (nSettings < 0) return false;
    const int32_t count = *(const int32_t*)(g_objects + 0x14);
    for (int32_t i = 0; i < count; ++i) {
        uint8_t* item = ObjectItem(i);
        uint8_t* obj = item ? *(uint8_t**)item : nullptr;
        if (!obj || (*(const uint32_t*)(obj + 0x08) & (kRfClassDefault | kRfArchetype))) continue;
        if (ClassOf(obj) && NameOf(ClassOf(obj)) == nSettings) { g_settings = obj; g_settingsIndex = i; break; }
    }
    if (!g_settings) return false;
    g_component = (uint8_t*)fnGetComponent(g_settings, true, false);
    SKY_INFO("coronas: GTAWorldSettings %p, corona component %p", g_settings, g_component);
    return g_component != nullptr;
}

// Ported from PC Show: GTA metres to UE cm with Y flipped, FColor BGRA.
void Show(Lamp& l, float scale, float intensity) {
    const float pos[3] = { l.pos[0] * 100.0f, -l.pos[1] * 100.0f, l.pos[2] * 100.0f };
    const uint32_t color = 0xFF000000u | l.r << 16 | l.g << 8 | l.b;
    if (l.corona < 0) {
        l.corona = fnAdd(g_component, pos[0], pos[1], pos[2], intensity, scale, color);
        ++g_shown;
    } else {
        fnUpdate(g_component, l.corona, pos, scale, color, intensity);
    }
    l.scale = scale;
    l.intensity = intensity;
}

void Hide(Lamp& l) {
    fnRemove(g_component, l.corona);
    l.corona = -1;
    --g_shown;
}

// p2dfx smooth blink: on/off ms, edges eased over up to 500 ms, one phase for all lamps of a mode.
float Pulse(uint32_t t, uint32_t on, uint32_t off) {
    const uint32_t phase = t % (on + off);
    if (phase >= on) return 0.0f;
    const float x = fminf(fminf((float)phase, (float)(on - phase)) / fminf(500.0f, on * 0.5f), 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// RegisterLODLights for one lamp: alpha 0..1 (0 = no corona) and radius in m. night = 0..255 time-of-day alpha.
float LampAlpha(const Lamp& l, const float* cam, float farClip, float night, uint32_t ms, float* radius) {
    if (l.pos[2] < -15.0f || l.pos[2] > 1030.0f) return 0.0f;
    const float dx = cam[0] - l.pos[0], dy = cam[1] - l.pos[1], dz = cam[2] - l.pos[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    const float nearEdge = l.drawDist - 30.0f; // the model's own corona covers the range below
    if (!l.noDist && (d <= nearEdge || d >= farClip)) return 0.0f;
    float r = 1.75f, a = 1.0f;
    if (!l.noDist) {
        r = fminf(1.75f * (d - nearEdge) / 30.0f, 1.75f);
        if (d < l.drawDist) a = (d - nearEdge) / 30.0f;
        else if (d > farClip - 100.0f) a = fminf(fmaxf((farClip - d) / 100.0f, 0.0f), 1.0f);
    }
    r *= fminf(1.0f + 3.0f * (d - nearEdge) / (1000.0f - nearEdge), 4.0f); // SlightlyIncreaseRadiusWithDistance
    // CoronaAlphaNearMinMult 0.5 up to 1 over 150 m, then x4 (FarBoostMax) over 900 m.
    const float past = d - nearEdge;
    a *= past > 150.0f ? 1.0f + 3.0f * fminf((past - 150.0f) / 900.0f, 1.0f) : 0.5f + 0.5f * fmaxf(past, 0.0f) / 150.0f;
    a *= night / 255.0f * l.a / 255.0f;
    r *= l.size * cfg.coronaSize;
    if (r > 1.0f) a *= fminf(fmaxf(1.0f / (0.75f * r + 0.25f), 0.3f), 1.0f);
    if (l.blink == 1) a *= Pulse(ms, 500, 500);
    else if (l.blink >= 2 && l.blink <= 7) a *= Pulse(ms, (l.blink - 1) * 1000, l.blink == 7 ? 4000 : (l.blink - 1) * 1000);
    *radius = r;
    return fminf(a, 1.0f);
}

// p2dfx night alpha: 20:00-07:00, 30 -> 255 over 20:00-21:00 ... 255 until 03:00 -> 30 at 07:00.
float NightAlpha() {
    const int t = *g_hours * 60 + *g_minutes;
    if (t >= 20 * 60) return fminf(15.0f / 16.0f * t - 1095.0f, 255.0f);
    if (t < 3 * 60) return 255.0f;
    if (t < 7 * 60) return -15.0f / 16.0f * t + 424.0f;
    return 0.0f;
}

void CoronasTick(void*, void*, float, float) {
    if (g_catching) {
        g_catching = false;
        g_dat.clear();
        SKY_INFO("coronas: %d lamps after level load", (int)g_lamps.size());
    }
    if (!g_ok || g_lamps.empty() || !skygfx::OnGameThread()) return;
    static unsigned frame = 0;
    ++frame;
    if (g_component && !Live()) { // world unloaded, component and coronas with it
        SKY_INFO("coronas: world settings gone (%d coronas dropped)", g_shown);
        g_component = nullptr;
        for (Lamp& l : g_lamps) l.corona = -1;
        g_shown = 0;
    }
    const float night = NightAlpha();
    const bool on = cfg.corona && *g_currArea == 0 && night > 0.0f;
    if (!g_component) {
        if (!on || frame % 120 != 1) return;
        if (!Resolve()) return;
    }
    if (!on && !g_shown) return;
    const float farClip = cfg.coronaFarClipDm > 0 ? cfg.coronaFarClipDm * 10.0f : kDefaultFarClip;
    const uint32_t ms = *g_timeMs;
    const float* cam = g_camMatrix + 12;
    for (Lamp& l : g_lamps) {
        float r = 0.0f;
        const float a = on ? LampAlpha(l, cam, farClip, night, ms, &r) : 0.0f;
        if (a < 1.0f / 255.0f) { if (l.corona >= 0) Hide(l); continue; }
        const float scale = r * 100.0f, intensity = a * cfg.coronaIntensity;
        if (l.corona < 0 || fabsf(scale - l.scale) > 0.03f * scale || fabsf(intensity - l.intensity) > 0.03f * fmaxf(intensity, l.intensity))
            Show(l, scale, intensity);
    }
}

void Init() {
    // shortcut: symbols that PC found by pattern scan are exported here; lamp natives and clock globals verified by name only.
    g_modelInfos = (uint8_t**)skygfx::Sym("_ZN10CModelInfo16ms_modelInfoPtrsE");
    const uint8_t* gobj = (const uint8_t*)skygfx::Sym("GUObjectArray");
    g_objects = gobj ? gobj + 0x10 : nullptr;
    g_hours = (const uint8_t*)skygfx::Sym("_ZN6CClock18ms_nGameClockHoursE");
    g_minutes = (const uint8_t*)skygfx::Sym("_ZN6CClock20ms_nGameClockMinutesE");
    g_timeMs = (const uint32_t*)skygfx::Sym("_ZN6CTimer22m_snTimeInMillisecondsE");
    g_currArea = (const uint8_t*)skygfx::Sym("_ZN5CGame8currAreaE");
    const uint8_t* camera = (const uint8_t*)skygfx::Sym("TheCamera");
    g_camMatrix = camera ? (const float*)(camera + 0x18) : nullptr; // unverified: PC matrix offset
    fnFNameCtor = (FNameCtor_Fn)skygfx::Sym("_ZN5FNameC1EPKc9EFindName");
    fnGetComponent = (GetComponent_Fn)skygfx::Sym("_ZN17AGTAWorldSettings18GetCoronaComponentEbb");
    fnAdd = (Add_Fn)skygfx::Sym("_ZN19UGTACoronaComponent9AddCoronaE7FVectorff6FColor");
    fnUpdate = (Update_Fn)skygfx::Sym("_ZN19UGTACoronaComponent12UpdateCoronaEiRK7FVectorf6FColorf");
    fnRemove = (Remove_Fn)skygfx::Sym("_ZN19UGTACoronaComponent12RemoveCoronaEi");
    void* load = skygfx::Sym("_ZN11CFileLoader18LoadObjectInstanceEPN10SanAndreas19CFileObjectInstanceEPKcb");
    if (!g_modelInfos || !g_objects || !g_hours || !g_minutes || !g_timeMs || !g_currArea || !g_camMatrix || !fnFNameCtor ||
        !fnGetComponent || !fnAdd || !fnUpdate || !fnRemove || !load) {
        SKY_LOG("coronas: required symbols missing, distant lamp coronas off");
        return;
    }
    const char* dir = aml->GetConfigPath();
    const int lights = LoadDat(dir);
    if (lights <= 0) { SKY_LOG("coronas: %s/SALodLights.dat missing or empty: distant lamp coronas off", dir); return; }
    aml->Hook(load, (void*)&Hooked_LoadInstance, (void**)&o_LoadInstance);
    SKY_INFO("coronas: %d lights for %d models from SALodLights.dat", lights, (int)g_dat.size());
    skygfx::Observe(skygfx::Point::Tick, &CoronasTick);
    g_catching = true;
    g_ok = true;
}

const skygfx::Feature kFeature = { "Coronas", Init, nullptr, true };  // early: hooks IPL loading
skygfx::Registrar g_registrar(kFeature);

}  // namespace
