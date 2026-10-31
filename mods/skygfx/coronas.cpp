// Distant lamp coronas, ported from PC coronas.cpp (Project2DFX SALodLights).
// SALodLights.dat lists corona offsets per model. Every IPL instance of those models that CFileLoader::
// LoadObjectInstance builds becomes a lamp (instance position + offset turned by the heading). Each frame, lamps past
// the model's own corona range and inside the timecyc far clip get a corona in DE's UGTACoronaComponent
// (AGTAWorldSettings::GetCoronaComponent(dynamic)), driven by its exported natives.
//
// Differences from PC, all for Android's streaming: instances are caught for the whole session (DE streams IPL
// sectors in after the first frame), deduplicated by position, from any thread (handed to the game thread under a
// mutex). Layouts verified on libUE4.so v1.87.0:
//   CFileObjectInstance (LoadObjectInstance 0x5450ff8): pos +0x0, quat xyzw +0xC, model u16 +0x1C, area +0x20.
//   CBaseModelInfo: key +0x08 (CModelInfo::GetModelInfo 0x556c3f4), draw distance +0x38 (CFileLoader::LoadObject 0x544758c).
//   AGTAWorldSettings corona components (GetCoronaComponent 0x4c8afa0): static +0x3B0, dynamic +0x3B8, up-close +0x3C0.
//   UGTACoronaComponent natives (0x4b85f80 Add, 0x4b868bc Update, 0x4b86ab4 Remove) all dereference the first mesh
//   section (TArray at +0x488, count +0x490) without a check, so nothing is called before the mesh exists.
//   TheCamera: CMatrix* at +0x18, position at matrix +0x30 (CCamera::Process 0x5426668).
// Game thread only, except Catch.
#include <mod/amlmod.h>
#include "skygfx.h"
#include <ctype.h>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace skygfx;
namespace {

namespace MDL { constexpr size_t Key = 0x08, DrawDistance = 0x38; }
namespace INST { constexpr size_t Model = 0x1C, Area = 0x20; }
namespace WS { constexpr size_t StaticCoronas = 0x3B0; }
namespace CORONA { constexpr size_t Sections = 0x488, SectionCount = 0x490; }
constexpr size_t kCamMatrix = 0x18, kCamPlacement = 0x08, kFarClip = 0x50;  // TheCamera, CColourSet
constexpr uint32_t kRfClassDefault = 0x10, kRfArchetype = 0x20;
constexpr uint32_t kItemUnreachable = 1u << 28, kItemPendingKill = 1u << 29;
constexpr float kFallbackFarClip = 1500.0f;  // only when the timecyc far clip is unreadable

struct Light { float off[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; };
struct Lamp { float pos[3]; uint8_t r, g, b, a; float size, drawDist; int blink, noDist; int32_t corona; float scale, intensity; };

std::unordered_map<uint32_t, std::vector<Light>> g_dat;  // by model key; built at load, read-only afterwards
std::vector<Lamp> g_lamps;                               // game thread
std::mutex g_pendingMutex;                               // guards g_pending and g_seen
std::vector<Lamp> g_pending;                             // caught instances not yet handed to the game thread
std::unordered_set<uint64_t> g_seen;                     // lamp positions already caught (decimetre grid)

uint8_t** g_modelInfos = nullptr;     // CModelInfo::ms_modelInfoPtrs
const uint8_t* g_objects = nullptr;   // GUObjectArray + 0x10 (FChunkedFixedUObjectArray)
const uint8_t* g_hours = nullptr;     // CClock::ms_nGameClockHours
const uint8_t* g_minutes = nullptr;   // CClock::ms_nGameClockMinutes
const uint32_t* g_timeMs = nullptr;   // CTimer::m_snTimeInMilliseconds
const int32_t* g_currArea = nullptr;  // CGame::currArea
const uint8_t* g_camera = nullptr;    // TheCamera
const uint8_t* g_colours = nullptr;   // CTimeCycle::m_CurrentColours
bool g_ok = false;
uint8_t* g_settings = nullptr;        // AGTAWorldSettings of the running world
int32_t g_settingsIndex = -1;
uint8_t* g_component = nullptr;       // its dynamic UGTACoronaComponent
int g_shown = 0;
int g_candidates = 0;                 // AGTAWorldSettings seen by the last Resolve
float g_lastNight = 0.0f, g_lastFarClip = 0.0f, g_lastCam[3] = {};

// Exported natives, called directly (game thread only). FVector is passed as three floats (HFA); AddCorona takes
// position s0-s2, intensity s3, scale s4 (half the quad size, UE units), FColor w1.
typedef void* (*GetComponent_Fn)(void* settings, bool dynamic, bool upClose);
typedef int32_t (*Add_Fn)(void* comp, float x, float y, float z, float intensity, float scale, uint32_t color);
typedef void (*Update_Fn)(void* comp, int32_t index, const float* pos, float scale, uint32_t color, float intensity);
typedef void (*Remove_Fn)(void* comp, int32_t index);
typedef void (*FNameCtor_Fn)(void* self, const char* s, int findType);
GetComponent_Fn fnGetComponent = nullptr;
Add_Fn fnAdd = nullptr;
Update_Fn fnUpdate = nullptr;
Remove_Fn fnRemove = nullptr;
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

// Position on a 10 cm grid, 21 bits per axis (+-104 km): the identity of a lamp across stream-ins.
uint64_t GridKey(const float* p) {
    uint64_t k = 0;
    for (int i = 0; i < 3; ++i) k = (k << 21) | ((uint64_t)(int64_t)lrintf(p[i] * 10.0f) & 0x1FFFFF);
    return k;
}

// Queues a lamp unless one already sits at that spot. Any thread.
void AddLamp(const Lamp& l) {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (g_seen.insert(GridKey(l.pos)).second) g_pending.push_back(l);
}

// SALodLights.dat: "%model" sections, then "R G B A offX offY offZ CustomSize DrawDistance Blink NoDistance
// Searchlight" (legacy lines lack DrawDistance). "%additional_coronas" offsets are world positions.
int LoadDat(const char* dir) {
    char path[512];
    const size_t n = strlen(dir);
    snprintf(path, sizeof(path), "%s%sSALodLights.dat", dir, (n && dir[n - 1] == '/') ? "" : "/");
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    std::vector<Light>* section = nullptr;
    bool world = false;
    int count = 0;
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
        else if (got != 12) continue;  // comment, blank
        // shortcut: traffic lights (CustomSize 0.45) skipped, upgrade with CTrafficLights phases and per-light road axis.
        if (l.size == 0.45f) continue;
        l.r = (uint8_t)r; l.g = (uint8_t)g; l.b = (uint8_t)b; l.a = (uint8_t)a;
        if (world) AddLamp({ { l.off[0], l.off[1], l.off[2] }, l.r, l.g, l.b, l.a, l.size, 0.0f, l.blink, l.noDist, -1, 0, 0 });
        else if (section) section->push_back(l);
        ++count;
    }
    fclose(f);
    return count;
}

// One IPL instance: lamps for every dat light of its model. Heading as LoadObjectInstance sets it for an
// untilted instance: acos(qw) x (qz < 0 ? 2 : -2). Any thread.
void Catch(const uint8_t* inst) {
    const uint8_t* mi = g_modelInfos[*(const uint16_t*)(inst + INST::Model)];
    if (!mi || inst[INST::Area] != 0) return;  // interiors never show outside
    auto it = g_dat.find(*(const uint32_t*)(mi + MDL::Key));
    if (it == g_dat.end()) return;
    const float* q = (const float*)inst;       // pos xyz, then quaternion xyzw at [3..6]
    if (q[0] == 0.0f && q[1] == 0.0f) return;  // no placement
    const float h = acosf(fminf(fmaxf(q[6], -1.0f), 1.0f)) * (q[5] < 0.0f ? 2.0f : -2.0f);
    const float c = cosf(h), s = sinf(h), dd = *(const float*)(mi + MDL::DrawDistance);
    for (const Light& l : it->second)
        AddLamp({ { q[0] + c * l.off[0] - s * l.off[1], q[1] + s * l.off[0] + c * l.off[1], q[2] + l.off[2] },
                  l.r, l.g, l.b, l.a, l.size, fminf(l.drawDist, dd), l.blink, l.noDist, -1, 0, 0 });
}

typedef uint8_t* (*LoadInstance_Fn)(const uint8_t* inst, const char* name, uint8_t fromIpl);
LoadInstance_Fn o_LoadInstance = nullptr;
uint8_t* Hooked_LoadInstance(const uint8_t* inst, const char* name, uint8_t fromIpl) {
    uint8_t* e = o_LoadInstance(inst, name, fromIpl);
    if (e) Catch(inst);
    return e;
}

// Moves caught lamps to the game thread's list. Never blocks the frame: a held lock waits for the next one.
void DrainPending() {
    std::unique_lock<std::mutex> lock(g_pendingMutex, std::try_to_lock);
    if (!lock.owns_lock() || g_pending.empty()) return;
    const size_t before = g_lamps.size();
    g_lamps.insert(g_lamps.end(), g_pending.begin(), g_pending.end());
    g_pending.clear();
    SKY_INFO("coronas: %d lamps (+%d)", (int)g_lamps.size(), (int)(g_lamps.size() - before));
}

// ---------------------------------------------------------------- UE side

// FUObjectItem: 0x18 stride, chunks of 65536 (verified by propfix).
uint8_t* ObjectItem(int32_t index) {
    if (!g_objects || index < 0 || index >= *(const int32_t*)(g_objects + 0x14)) return nullptr;
    uint8_t* const* chunks = *(uint8_t* const* const*)g_objects;
    uint8_t* chunk = chunks ? chunks[index >> 16] : nullptr;
    return chunk ? chunk + (index & 0xFFFF) * 0x18 : nullptr;
}

bool ItemDead(const uint8_t* item) { return (*(const uint32_t*)(item + 8) & (kItemUnreachable | kItemPendingKill)) != 0; }

// UObject: Flags +0x08, ClassPrivate +0x10, NamePrivate +0x18 (UE 4.26).
int32_t NameOf(const uint8_t* obj) { return *(const int32_t*)(obj + 0x18); }
const uint8_t* ClassOf(const uint8_t* obj) { return *(const uint8_t* const*)(obj + 0x10); }

int32_t FindName(const char* s) {
    struct { int32_t index, number; } n = { 0, 0 };
    fnFNameCtor(&n, s, 0 /* FNAME_Find */);
    return n.index > 0 ? n.index : -1;
}

bool SettingsLive() {
    const uint8_t* item = ObjectItem(g_settingsIndex);
    return item && *(uint8_t* const*)item == g_settings && !ItemDead(item);
}

// The corona natives need the component's first mesh section (see header).
bool MeshReady() {
    return *(const int32_t*)(g_component + CORONA::SectionCount) > 0 && *(void* const*)(g_component + CORONA::Sections);
}

// GObjects pass: the AGTAWorldSettings whose static corona component DE's lights already use (the running world's;
// menu and streaming sub-levels have none), as PC.
bool Resolve() {
    const int32_t nSettings = FindName("GTAWorldSettings");
    if (nSettings < 0) return false;
    uint8_t* best = nullptr;
    int32_t bestIndex = -1;
    g_candidates = 0;
    const int32_t count = *(const int32_t*)(g_objects + 0x14);
    for (int32_t i = 0; i < count; ++i) {
        uint8_t* item = ObjectItem(i);
        uint8_t* obj = item ? *(uint8_t**)item : nullptr;
        if (!obj || ItemDead(item) || (*(const uint32_t*)(obj + 0x08) & (kRfClassDefault | kRfArchetype))) continue;
        if (!ClassOf(obj) || NameOf(ClassOf(obj)) != nSettings) continue;
        ++g_candidates;
        if (!best || *(void* const*)(obj + WS::StaticCoronas)) { best = obj; bestIndex = i; }
    }
    if (!best) return false;
    uint8_t* comp = (uint8_t*)fnGetComponent(best, true, false);
    SKY_INFO("coronas: GTAWorldSettings %p (%d candidates), corona component %p", best, g_candidates, comp);
    if (!comp) return false;
    g_settings = best;
    g_settingsIndex = bestIndex;
    g_component = comp;
    return true;
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
    const float nearEdge = l.drawDist - 30.0f;  // the model's own corona covers the range below
    if (!l.noDist && (d <= nearEdge || d >= farClip)) return 0.0f;
    float r = 1.75f, a = 1.0f;
    if (!l.noDist) {
        r = fminf(1.75f * (d - nearEdge) / 30.0f, 1.75f);
        if (d < l.drawDist) a = (d - nearEdge) / 30.0f;
        else if (d > farClip - 100.0f) a = fminf(fmaxf((farClip - d) / 100.0f, 0.0f), 1.0f);
    }
    r *= fminf(1.0f + 3.0f * (d - nearEdge) / (1000.0f - nearEdge), 4.0f);  // SlightlyIncreaseRadiusWithDistance
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

// p2dfx night alpha: 30 at 20:00 rising linearly to 255 at 24:00, 255 until 03:00, falling to 30 at 07:00.
float NightAlpha(int hours, int minutes) {
    const int t = hours * 60 + minutes;
    if (t >= 20 * 60) return fminf(15.0f / 16.0f * t - 1095.0f, 255.0f);
    if (t < 3 * 60) return 255.0f;
    if (t < 7 * 60) return -15.0f / 16.0f * t + 424.0f;
    return 0.0f;
}

// Camera position in GTA metres: the matrix row 3 when the camera has a matrix, else its simple placement.
const float* CameraPos() {
    const float* m = *(const float* const*)(g_camera + kCamMatrix);
    return m ? m + 12 : (const float*)(g_camera + kCamPlacement);
}

// The timecyc far clip (CColourSet +0x50, device-verified), as PC's g_look.farClip.
float FarClip() {
    if (cfg.coronaFarClipDm > 0) return cfg.coronaFarClipDm * 10.0f;
    const float f = g_colours ? *(const float*)(g_colours + kFarClip) : 0.0f;
    return f > 50.0f && f < 10000.0f ? f : kFallbackFarClip;
}

void DropComponent(const char* why) {
    if (g_component) SKY_INFO("coronas: %s (%d coronas dropped)", why, g_shown);
    g_component = nullptr;
    g_settings = nullptr;
    g_settingsIndex = -1;
    for (Lamp& l : g_lamps) l.corona = -1;
    g_shown = 0;
}

void CoronasTick(void*, void*, float, float) {
    if (!g_ok || !OnGameThread()) return;
    DrainPending();
    if (g_lamps.empty()) return;
    static unsigned frame = 0;
    ++frame;
    if (g_component && !SettingsLive()) DropComponent("world settings gone");  // world unloaded, coronas with it
    const float night = NightAlpha(*g_hours, *g_minutes);
    const bool on = cfg.enabled && cfg.corona && *g_currArea == 0 && night > 0.0f;
    g_lastNight = night;
    if (!g_component) {
        if (!on || frame % 120 != 1) return;
        if (!Resolve()) return;
    }
    if (!MeshReady()) return;  // component created this frame; its mesh comes with its first tick
    if (!on && !g_shown) return;
    const float farClip = FarClip();
    const float* cam = CameraPos();
    const uint32_t ms = *g_timeMs;
    g_lastFarClip = farClip;
    memcpy(g_lastCam, cam, sizeof g_lastCam);
    for (Lamp& l : g_lamps) {
        float r = 0.0f;
        const float a = on ? LampAlpha(l, cam, farClip, night, ms, &r) : 0.0f;
        if (a < 1.0f / 255.0f) { if (l.corona >= 0) Hide(l); continue; }
        const float scale = r * 100.0f, intensity = a * cfg.coronaIntensity;
        if (l.corona < 0 || fabsf(scale - l.scale) > 0.03f * scale || fabsf(intensity - l.intensity) > 0.03f * fmaxf(intensity, l.intensity))
            Show(l, scale, intensity);
    }
}

void Status() {
    if (!g_ok) { SKY_LOG("coronas: off"); return; }
    size_t pending;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        pending = g_pending.size();
    }
    SKY_LOG("coronas: lamps=%d pending=%d shown=%d night=%.0f clock=%02d:%02d area=%d farclip=%.0f cam=%.0f,%.0f,%.0f "
            "settings=%p candidates=%d component=%p mesh=%d",
            (int)g_lamps.size(), (int)pending, g_shown, g_lastNight, *g_hours, *g_minutes, *g_currArea, g_lastFarClip,
            g_lastCam[0], g_lastCam[1], g_lastCam[2], g_settings, g_candidates, g_component,
            g_component ? *(const int32_t*)(g_component + CORONA::SectionCount) : -1);
}

// Self-check of the pure helpers against hand-computed p2dfx values.
bool SelfCheck() {
    const float a = NightAlpha(20, 0), b = NightAlpha(21, 0), c = NightAlpha(12, 0), d = NightAlpha(5, 0);
    const float p[3] = { -1.0f, 2.5f, 1000.0f }, q[3] = { -1.0f, 2.5f, 1000.04f };
    return fabsf(a - 30.0f) < 0.01f && fabsf(b - 86.25f) < 0.01f && c == 0.0f && fabsf(d - 142.75f) < 0.01f &&
           GridKey(p) == GridKey(q);
}

void Init() {
    g_modelInfos = (uint8_t**)Sym("_ZN10CModelInfo16ms_modelInfoPtrsE");
    const uint8_t* gobj = (const uint8_t*)Sym("GUObjectArray");
    g_objects = gobj ? gobj + 0x10 : nullptr;
    g_hours = (const uint8_t*)Sym("_ZN6CClock18ms_nGameClockHoursE");
    g_minutes = (const uint8_t*)Sym("_ZN6CClock20ms_nGameClockMinutesE");
    g_timeMs = (const uint32_t*)Sym("_ZN6CTimer22m_snTimeInMillisecondsE");
    g_currArea = (const int32_t*)Sym("_ZN5CGame8currAreaE");
    g_camera = (const uint8_t*)Sym("TheCamera");
    g_colours = (const uint8_t*)Sym("_ZN10CTimeCycle16m_CurrentColoursE");
    fnFNameCtor = (FNameCtor_Fn)Sym("_ZN5FNameC1EPKc9EFindName");
    fnGetComponent = (GetComponent_Fn)Sym("_ZN17AGTAWorldSettings18GetCoronaComponentEbb");
    fnAdd = (Add_Fn)Sym("_ZN19UGTACoronaComponent9AddCoronaE7FVectorff6FColor");
    fnUpdate = (Update_Fn)Sym("_ZN19UGTACoronaComponent12UpdateCoronaEiRK7FVectorf6FColorf");
    fnRemove = (Remove_Fn)Sym("_ZN19UGTACoronaComponent12RemoveCoronaEi");
    void* load = Sym("_ZN11CFileLoader18LoadObjectInstanceEPN10SanAndreas19CFileObjectInstanceEPKcb");
    if (!g_modelInfos || !g_objects || !g_hours || !g_minutes || !g_timeMs || !g_currArea || !g_camera || !fnFNameCtor ||
        !fnGetComponent || !fnAdd || !fnUpdate || !fnRemove || !load) {
        SKY_LOG("coronas: required symbols missing, distant lamp coronas off");
        return;
    }
    if (!SelfCheck()) {
        SKY_LOG("coronas: self-check failed, distant lamp coronas off");
        return;
    }
    const int lights = LoadDat(aml->GetConfigPath());
    if (lights <= 0) {
        SKY_LOG("coronas: %s/SALodLights.dat missing or empty, distant lamp coronas off", aml->GetConfigPath());
        return;
    }
    if (!aml->Hook(load, (void*)&Hooked_LoadInstance, (void**)&o_LoadInstance)) {
        SKY_LOG("coronas: LoadObjectInstance hook failed, distant lamp coronas off");
        return;
    }
    SKY_LOG("coronas: %d lights for %d models from SALodLights.dat", lights, (int)g_dat.size());
    Observe(Point::Tick, &CoronasTick);
    g_ok = true;
}

const Feature kFeature = { "coronas", Init, nullptr, true, Status };  // early: hooks IPL loading
Registrar g_registrar(kFeature);

}  // namespace
