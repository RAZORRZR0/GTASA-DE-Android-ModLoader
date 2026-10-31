// Debug weather and clock tools (port map 2.15): forced / blended weather, set time, freeze, speed, timelapse.
// Freecam, noclip and move-to-camera ported from SkyGfxDE; movement comes from the pad walk stick.
#include <stdint.h>
#include <atomic>
#include <math.h>
#include <string.h>
#include <mod/amlmod.h>
#include "skygfx.h"

using namespace skygfx;

namespace {

constexpr int kWeathers = 23;
constexpr float kTimelapseSpeed = 240.0f; // 240x: a game day in 6 real seconds

enum WeatherMode { WM_GAME, WM_FORCE, WM_BLEND };

// Written from configChanged (ModMenu thread), consumed on the game thread in the CWeather::Update hook.
std::atomic<int> g_weatherMode{WM_GAME};
std::atomic<int> g_weatherA{0}, g_weatherB{0};
std::atomic<float> g_blend{0.0f};
std::atomic<int> g_setHour{-1}, g_setMinute{0};
std::atomic<bool> g_freezeTime{false}, g_timelapse{false};
std::atomic<float> g_timeSpeed{1.0f};

// Game-thread only.
bool g_weatherWasForced = false;
uint32_t g_baseMsPerMinute = 0;

int16_t* g_oldWeather = nullptr;     // CWeather::OldWeatherType
int16_t* g_newWeather = nullptr;     // CWeather::NewWeatherType
int16_t* g_forcedWeather = nullptr;  // CWeather::ForcedWeatherType (-1 = none)
float* g_interp = nullptr;           // CWeather::InterpolationValue
uint32_t* g_msPerMinute = nullptr;   // CClock::ms_nMillisecondsPerGameMinute
uint32_t* g_lastTick = nullptr;      // CClock::ms_nLastClockTick
uint16_t* g_seconds = nullptr;       // CClock::ms_nGameClockSeconds
uint32_t* g_timeMs = nullptr;        // CTimer::m_snTimeInMilliseconds

typedef void (*SetGameClock_Fn)(uint8_t hours, uint8_t minutes, uint8_t day);
typedef void (*Update_Fn)();
SetGameClock_Fn g_SetGameClock = nullptr;
Update_Fn o_WeatherUpdate = nullptr;

// Mode: forceWeather < 0 leaves the game in charge; with blendWeatherB >= 0 the two types are blended.
void Pending_Weather() {
    if (cfg.forceWeather < 0) {
        g_weatherMode.store(WM_GAME);
    } else if (cfg.blendWeatherB >= 0) {
        g_weatherA.store(cfg.forceWeather);
        g_weatherB.store(cfg.blendWeatherB);
        g_blend.store(cfg.blend);
        g_weatherMode.store(WM_BLEND);
    } else {
        g_weatherA.store(cfg.forceWeather);
        g_weatherMode.store(WM_FORCE);
    }
}

void BeforeWeather() {
    const int hour = g_setHour.exchange(-1);
    if (hour >= 0) g_SetGameClock((uint8_t)hour, (uint8_t)g_setMinute.load(), 0);
    if (!g_baseMsPerMinute && *g_msPerMinute) g_baseMsPerMinute = *g_msPerMinute;
    if (g_baseMsPerMinute) {
        const float want = g_timelapse.load() ? kTimelapseSpeed : g_timeSpeed.load();
        const float speed = want < 0.05f ? 0.05f : want;
        *g_msPerMinute = (uint32_t)(g_baseMsPerMinute / speed + 0.5f);
    }
    if (g_freezeTime.load() && !g_timelapse.load()) { *g_lastTick = *g_timeMs; *g_seconds = 0; }
}

void AfterWeather() {
    const int mode = g_weatherMode.load();
    if (mode == WM_GAME) {
        if (g_weatherWasForced) { *g_forcedWeather = -1; g_weatherWasForced = false; }
        return;
    }
    g_weatherWasForced = true;
    const int a = g_weatherA.load() % kWeathers;
    if (mode == WM_FORCE) {
        *g_oldWeather = *g_newWeather = *g_forcedWeather = (int16_t)a;
    } else {
        const int b = g_weatherB.load() % kWeathers;
        *g_oldWeather = (int16_t)a;
        *g_newWeather = (int16_t)b;
        *g_forcedWeather = (int16_t)b;
        *g_interp = g_blend.load();
    }
}

void Hooked_WeatherUpdate() {
    BeforeWeather();
    o_WeatherUpdate();
    AfterWeather();
}

// ---------------------------------------------------------------- freecam / noclip / move-to-camera
// Offsets (docs/arm64_map.md): CEntity matrix ptr +0x18, flags +0x30; CPhysical m_vecMoveSpeed +0x78, m_vecTurnSpeed +0x84.
// bUsesCollision = flags bit 0x01 at +0x30: CWorld::TestSphereAgainstSectorList (0x54be61c) and
// CWorld::ProcessLineOfSightSectorList (0x54b3698) do `ldrb w8,[entity,#0x30]; tbz w8,#0` before a collision test.
// TheCamera (0xaac6700): CMatrix* m_matrix at +0x18; CCamera::Process (0x5426668) loads [this,#0x18] and reads position at matrix +0x30.
// FindPlayerEntity(-1) / FindPlayerVehicle(-1,false): a negative index selects the default player (csel ... lt at 0x548abf0 and 0x547fe78).
// CPad::GetPedWalkLeftRight/UpDown(this, bool, int) (0x546addc, 0x546af9c): bool false returns 0 (tbz w0 at 0x546aef4), so pass true.
constexpr uint32_t kOffMatrix = 0x18, kOffFlags = 0x30, kOffMoveSpeed = 0x78, kOffTurnSpeed = 0x84;
constexpr uint8_t kUsesCollision = 0x01;
constexpr uint32_t kOffCamMatrix = 0x18;

// Written from configChanged (ModMenu thread), read on the game thread.
std::atomic<bool> g_freecamOn{false}, g_noclipOn{false}, g_moveToCamReq{false};
std::atomic<float> g_freecamSpeed{20.0f}, g_noclipSpeed{15.0f};

// Game-thread only.
uint8_t* g_theCamera = nullptr;     // TheCamera (CCamera object)
float* g_timeStep = nullptr;        // CTimer::ms_fTimeStep (1.0 per frame at 30 fps)
bool g_freecamInit = false;
float g_camPos[3] = {};
uint8_t* g_noclipEntity = nullptr;
bool g_noclipHadCollision = false;

typedef uint8_t* (*FindPlayerEntity_Fn)(int);
typedef uint8_t* (*FindPlayerVehicle_Fn)(int, bool);
typedef uint8_t* (*GetPad_Fn)(int);
typedef int32_t (*PadAxis_Fn)(void* pad, bool, int);
typedef void (*CameraProcess_Fn)(void* self);
typedef void (*DerivedValues_Fn)(void* self, bool, bool);
typedef void (*CopyToRw_Fn)(void* self, bool);
FindPlayerEntity_Fn g_FindPlayerEntity = nullptr;
FindPlayerVehicle_Fn g_FindPlayerVehicle = nullptr;
GetPad_Fn g_GetPad = nullptr;
PadAxis_Fn o_PadLeftRight = nullptr, o_PadUpDown = nullptr;
CameraProcess_Fn o_CameraProcess = nullptr;
// CCamera::Process ends with CalculateDerivedValues(this, false, true) (0x5427ad8) and CopyCameraMatrixToRWCam(this,
// false) (0x5427ae4), which hands TheCamera to UE's view. A matrix written after Process returns is only rendered if
// both run again with it.
DerivedValues_Fn g_CalculateDerivedValues = nullptr;
CopyToRw_Fn g_CopyCameraMatrixToRWCam = nullptr;
const uint8_t* g_cutsceneRunning = nullptr;  // CCutsceneMgr::ms_running
constexpr uint32_t kOffLodMult = 0x104;      // TheCamera.m_fLODDistMultiplier (CCamera::Process 0x5427b40: 70 / FOV)

float* CameraMatrix() { return g_theCamera ? *(float**)(g_theCamera + kOffCamMatrix) : nullptr; }

// Yaw/pitch of the camera's forward row (matrix +0x10..+0x20).
void CameraAngles(const float* m, float& yaw, float& pitch) {
    const float* fwd = m + 4;
    yaw = atan2f(fwd[1], fwd[0]);
    pitch = asinf(fmaxf(-1.0f, fminf(1.0f, fwd[2])));
}

// Pad stick -> world direction. fly includes pitch; otherwise horizontal only (noclip).
bool StickDir(float yaw, float pitch, bool fly, float v[3]) {
    uint8_t* pad = g_GetPad(0);
    if (!pad) return false;
    // shortcut: stick scale /128 and forward = -UpDown are unverified on device; check on hardware.
    const float sx = fmaxf(-1.0f, fminf(1.0f, o_PadLeftRight(pad, true, 0) / 128.0f));
    const float sy = fmaxf(-1.0f, fminf(1.0f, -o_PadUpDown(pad, true, 0) / 128.0f));
    if (fabsf(sx) < 0.1f && fabsf(sy) < 0.1f) return false;
    if (!fly) pitch = 0.0f;
    const float cy = cosf(yaw), sn = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float fwd[3] = {cp * cy, cp * sn, sp};
    const float right[3] = {sn, -cy, 0.0f};
    for (int i = 0; i < 3; i++) v[i] = fwd[i] * sy + right[i] * sx;
    return true;
}

// Restores collision only on an entity known alive (the player's current ped or vehicle): a vehicle left or
// destroyed during noclip may already be freed. shortcut: a vehicle left mid-noclip keeps collision off.
void RestoreNoclipEntity(const uint8_t* ped, const uint8_t* veh) {
    if (!g_noclipEntity) return;
    if (g_noclipEntity == ped || g_noclipEntity == veh) {
        if (g_noclipHadCollision) g_noclipEntity[kOffFlags] |= kUsesCollision;
        memset(g_noclipEntity + kOffMoveSpeed, 0, 12);
        memset(g_noclipEntity + kOffTurnSpeed, 0, 12);
    }
    g_noclipEntity = nullptr;
}

void AfterCamera(float dt) {
    float* cm = CameraMatrix();
    uint8_t* ped = g_FindPlayerEntity(-1);
    uint8_t* veh = g_FindPlayerVehicle(-1, false);
    uint8_t* ent = veh ? veh : ped;
    const bool freecam = g_freecamOn.load();

    if (g_noclipOn.load() && !freecam && ent && cm) {
        if (ent != g_noclipEntity) {
            RestoreNoclipEntity(ped, veh);
            g_noclipEntity = ent;
            g_noclipHadCollision = (ent[kOffFlags] & kUsesCollision) != 0;
        }
        ent[kOffFlags] &= ~kUsesCollision;
        memset(ent + kOffMoveSpeed, 0, 12);
        memset(ent + kOffTurnSpeed, 0, 12);
        float* em = *(float**)(ent + kOffMatrix);
        float yaw, pitch;
        CameraAngles(cm, yaw, pitch);
        float v[3];
        if (em && StickDir(yaw, pitch, false, v)) {
            const float s = g_noclipSpeed.load() * dt;
            for (int i = 0; i < 3; i++) em[12 + i] += v[i] * s;
        }
    } else {
        RestoreNoclipEntity(ped, veh);
    }

    if (freecam && cm) {
        float yaw, pitch;
        CameraAngles(cm, yaw, pitch);
        if (!g_freecamInit) { memcpy(g_camPos, cm + 12, 12); g_freecamInit = true; }
        float v[3];
        if (StickDir(yaw, pitch, true, v)) {
            const float s = g_freecamSpeed.load() * dt;
            for (int i = 0; i < 3; i++) g_camPos[i] += v[i] * s;
        }
        // Rebuild the camera basis from yaw/pitch, as the PC mod does.
        const float cy = cosf(yaw), sn = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        cm[0] = sn;  cm[1] = -cy; cm[2] = 0.0f;
        cm[4] = cp * cy; cm[5] = cp * sn; cm[6] = sp;
        cm[8] = -sp * cy; cm[9] = -sp * sn; cm[10] = cp;
        memcpy(cm + 12, g_camPos, 12);
        g_CalculateDerivedValues(g_theCamera, false, true);
        g_CopyCameraMatrixToRWCam(g_theCamera, false);
    } else {
        g_freecamInit = false;
    }

    if (g_moveToCamReq.exchange(false) && ent && cm) {
        float* em = *(float**)(ent + kOffMatrix);
        if (em) {
            memcpy(em + 12, cm + 12, 12);
            memset(ent + kOffMoveSpeed, 0, 12);
        }
    }
}

// PC tools.cpp Hooked_CameraProcess: [World] LodDistance scales DE's m_fLODDistMultiplier (DE dropped the PC draw
// distance factor from 70 / FOV) before this frame's render list is built; cutscenes keep DE's value, as the original.
void Hooked_CameraProcess(void* self) {
    o_CameraProcess(self);
    if (cfg.enabled && !*g_cutsceneRunning) *(float*)(g_theCamera + kOffLodMult) *= cfg.lodDistance;
    AfterCamera(*g_timeStep / 30.0f);
}

// Zero the walk stick while freecam is on so the player does not walk.
int32_t Hooked_PadLeftRight(void* self, bool b, int i) {
    const int32_t v = o_PadLeftRight(self, b, i);
    return g_freecamOn.load() ? 0 : v;
}
int32_t Hooked_PadUpDown(void* self, bool b, int i) {
    const int32_t v = o_PadUpDown(self, b, i);
    return g_freecamOn.load() ? 0 : v;
}

void InstallFreecam() {
    void* camProcess = Sym("_ZN7CCamera7ProcessEv");
    void* padLR = Sym("_ZN4CPad19GetPedWalkLeftRightEbi");
    void* padUD = Sym("_ZN4CPad16GetPedWalkUpDownEbi");
    g_FindPlayerEntity = (FindPlayerEntity_Fn)Sym("_Z16FindPlayerEntityi");
    g_FindPlayerVehicle = (FindPlayerVehicle_Fn)Sym("_Z17FindPlayerVehicleib");
    g_GetPad = (GetPad_Fn)Sym("_ZN4CPad6GetPadEi");
    g_theCamera = (uint8_t*)Sym("TheCamera");
    g_timeStep = (float*)Sym("_ZN6CTimer12ms_fTimeStepE");
    g_CalculateDerivedValues = (DerivedValues_Fn)Sym("_ZN7CCamera22CalculateDerivedValuesEbb");
    g_CopyCameraMatrixToRWCam = (CopyToRw_Fn)Sym("_ZN7CCamera23CopyCameraMatrixToRWCamEb");
    g_cutsceneRunning = (const uint8_t*)Sym("_ZN12CCutsceneMgr10ms_runningE");
    if (!camProcess || !padLR || !padUD || !g_FindPlayerEntity || !g_FindPlayerVehicle || !g_GetPad || !g_theCamera ||
        !g_timeStep || !g_CalculateDerivedValues || !g_CopyCameraMatrixToRWCam || !g_cutsceneRunning) {
        SKY_LOG("tools: freecam/noclip/LOD symbols missing, disabled");
        return;
    }
    if (!aml->Hook(camProcess, (void*)&Hooked_CameraProcess, (void**)&o_CameraProcess) ||
        !aml->Hook(padLR, (void*)&Hooked_PadLeftRight, (void**)&o_PadLeftRight) ||
        !aml->Hook(padUD, (void*)&Hooked_PadUpDown, (void**)&o_PadUpDown)) {
        SKY_LOG("tools: freecam/noclip hook failed");
        return;
    }
    SKY_LOG("tools: freecam/noclip installed");
}

void ToolsInit() {
    InstallFreecam();
    struct Entry { const char* sym; void** out; };
    void* weatherUpdate = Sym("_ZN8CWeather6UpdateEv");
    const Entry data[] = {
        {"_ZN8CWeather14OldWeatherTypeE", (void**)&g_oldWeather},
        {"_ZN8CWeather14NewWeatherTypeE", (void**)&g_newWeather},
        {"_ZN8CWeather17ForcedWeatherTypeE", (void**)&g_forcedWeather},
        {"_ZN8CWeather18InterpolationValueE", (void**)&g_interp},
        {"_ZN6CClock29ms_nMillisecondsPerGameMinuteE", (void**)&g_msPerMinute},
        {"_ZN6CClock17ms_nLastClockTickE", (void**)&g_lastTick},
        {"_ZN6CClock20ms_nGameClockSecondsE", (void**)&g_seconds},
        {"_ZN6CTimer22m_snTimeInMillisecondsE", (void**)&g_timeMs},
    };
    void* setClock = Sym("_ZN6CClock12SetGameClockEhhh");
    bool ok = weatherUpdate && setClock;
    for (const Entry& e : data) {
        *e.out = Sym(e.sym);
        if (!*e.out) { SKY_LOG("tools: missing %s", e.sym); ok = false; }
    }
    if (!ok) { SKY_LOG("tools: weather/clock symbols missing, tools disabled"); return; }
    g_SetGameClock = (SetGameClock_Fn)setClock;
    if (!aml->Hook(weatherUpdate, (void*)&Hooked_WeatherUpdate, (void**)&o_WeatherUpdate)) {
        SKY_LOG("tools: CWeather::Update hook failed");
        return;
    }
    SKY_LOG("tools: installed");
}

// One-shots (set time, move player to camera) fire only when their ini value changes while the game runs: not at
// startup (a save would get its clock reset every launch) and not on unrelated reloads.
void ToolsConfigChanged() {
    static bool seen = false;
    static int lastHour, lastMinute;
    static bool lastMove;
    Pending_Weather();
    if (seen && (cfg.setHour != lastHour || cfg.setMinute != lastMinute) && cfg.setHour >= 0) {
        g_setMinute.store(cfg.setMinute < 0 ? 0 : cfg.setMinute);
        g_setHour.store(cfg.setHour);
    }
    if (seen && cfg.movePlayerToCamera && !lastMove) g_moveToCamReq.store(true);
    lastHour = cfg.setHour;
    lastMinute = cfg.setMinute;
    lastMove = cfg.movePlayerToCamera;
    seen = true;
    g_freezeTime.store(cfg.freezeTime);
    g_timeSpeed.store(cfg.timeSpeed);
    g_timelapse.store(cfg.timelapse);
    g_freecamOn.store(cfg.freecam);
    g_noclipOn.store(cfg.noclip);
    g_freecamSpeed.store((float)cfg.freecamSpeed);
    g_noclipSpeed.store((float)cfg.noclipSpeed);
}

void ToolsStatus() {
    if (!g_SetGameClock) { SKY_LOG("tools: off"); return; }
    static const uint8_t* hours = (const uint8_t*)Sym("_ZN6CClock18ms_nGameClockHoursE");
    static const uint8_t* minutes = (const uint8_t*)Sym("_ZN6CClock20ms_nGameClockMinutesE");
    const uint8_t* ped = g_FindPlayerEntity ? g_FindPlayerEntity(-1) : nullptr;
    const float* pm = ped ? *(float* const*)(ped + kOffMatrix) : nullptr;
    const float* cm = CameraMatrix();
    const float zero[16] = {};
    if (!pm) pm = zero;
    if (!cm) cm = zero;
    SKY_LOG("tools: clock=%02d:%02d msPerMinute=%u weather=%d/%d forced=%d interp=%.2f freecam=%d noclip=%d player=%p "
            "ppos=%.1f,%.1f,%.1f cpos=%.1f,%.1f,%.1f",
            hours ? *hours : -1, minutes ? *minutes : -1, *g_msPerMinute, *g_oldWeather, *g_newWeather, *g_forcedWeather,
            *g_interp, (int)g_freecamOn.load(), (int)g_noclipOn.load(), ped, pm[12], pm[13], pm[14], cm[12], cm[13], cm[14]);
}

}  // namespace

static const Registrar kToolsFeature({"tools", ToolsInit, ToolsConfigChanged, false, ToolsStatus});
