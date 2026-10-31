// Post-effect inputs, published once per game frame for vkfx.cpp: a port of PC SkyGfxDE tools.cpp SpeedFxRowUpdate
// (SpeedFX row, rain grain, water-drop flags) and its water-drop source hooks. Game thread, except the hooks.
//
// arm64 v1.87.0, verified by disassembly:
//   TheCamera: m_nActiveCam u8 +0x5B, m_aCams stride 0x1B8, m_nMode u16 +0x1B0, m_nDirectionWasLooking u32 +0x1C4
//     (CCamera::GetLookDirection 0x54252bc: ldrb [x0,#0x5b]; madd 0x1b8; ldrh [..,#0x1b0]; ldr [..,#0x1c4]); CMatrix* +0x18.
//   CVehicle m_nVehicleSubType +0x894 (CHeli ctor stores 3, CPlane 4); CAutomobile handling flags +0x5C0, tire
//     temperature +0xC0C, gas pedal +0x710 (CAutomobile::NitrousControl 0x57e6ea8); CPhysical move speed +0x78.
//   Splash FX by name (adrp/add string references): "water_splash_big" Fx_c::TriggerWaterSplash 0x4af9650,
//     "water_splash" Fx_c::TriggerBulletSplash 0x4af9868, "water_splsh_sml" Fx_c::TriggerFootSplash 0x4af99fc.
//   Fx_c::InitStaticSystems 0x4af3854 (x19 = this = g_fx): prt_boatsplash +0x20, prt_wake +0x90, prt_watersplash +0x98.
//   CAEFireAudioEntity::AddAudioEvent 0x52b6910: event 137 (0x89) goes to PlayWaterSounds (hydrants, fountains).
#include <mod/amlmod.h>
#include <math.h>
#include <mutex>
#include <string.h>
#include "fx.h"
#include "skygfx.h"

namespace skygfx {
namespace {

constexpr uint32_t kCamMatrix = 0x18, kActiveCam = 0x5B, kCamStride = 0x1B8, kCamMode = 0x1B0, kCamLook = 0x1C4;
constexpr uint32_t kMatrix = 0x18, kMoveSpeed = 0x78, kSubType = 0x894;
constexpr uint32_t kHandlingFlags = 0x5C0, kTireTemp = 0xC0C, kGasPedal = 0x710, kNitroFlag = 0x80000;
constexpr uint32_t kPrtBoatSplash = 0x20, kPrtWake = 0x90, kPrtWaterSplash = 0x98;
constexpr uint32_t kHighLightMinIntensity = 0x9C;  // CColourSet, int32 (P4 dump: 54 00 00 00)
constexpr int kWaterAudioEvent = 137;

// PC kSpeedFx speeds (gta_sa.exe 1.0 US 0x8D5190); the passes/shift/wobble half lives in vkfx.cpp.
constexpr float kSpeedFxSpeed[7] = { 0.6f, 0.7f, 0.8f, 0.9f, 0.93f, 0.96f, 1.0f };

typedef void* (*FindVehicleFn)(int playerNum, bool includeRemote);
FindVehicleFn g_findVehicle;
const uint8_t* g_camera;         // TheCamera
const uint8_t* g_cutscene;       // CCutsceneMgr::ms_running
const uint8_t* g_colours;        // CTimeCycle::m_CurrentColours
const float* g_rain;             // CWeather::Rain
const float* g_underWater;       // CWeather::UnderWaterness
const int32_t* g_currArea;       // CGame::currArea
const uint32_t* g_zoneFlagsP;    // CCullZones::CurrentFlags_Player (bit 3 = no rain)
const uint32_t* g_zoneFlagsC;    // CCullZones::CurrentFlags_Camera
const int32_t* g_exitEnterState; // CEntryExitManager::ms_exitEnterState
uint8_t* g_fxObj;                // g_fx (Fx_c)

std::mutex g_mu;  // g_latest
FxState g_latest;
float g_camPos[3];  // the last published camera position, for the splash sources (under g_mu)

int SpeedFxInputRow(float input) {
    for (int i = 6; i >= 0; --i)
        if (input >= kSpeedFxSpeed[i]) return i;
    return -1;
}

// PC SpeedFxRowUpdate's row part: test mode, or the player's vehicle (not heli/plane/boat/train); nitro burning uses
// clamp(2 * (moveSpeed . forward) * (gas + 1), 0, 1) when the dot exceeds 0.2, even in cutscenes; otherwise |moveSpeed|
// unless a cutscene runs.
int SpeedFxRow() {
    if (!cfg.enabled || !cfg.speedFx) return -1;
    if (cfg.speedFxTest) return SpeedFxInputRow(1.0f);
    const uint8_t* veh = (const uint8_t*)g_findVehicle(-1, false);
    if (!veh) return -1;
    const uint32_t type = *(const uint32_t*)(veh + kSubType);
    if (type >= 3 && type <= 6) return -1;
    const float* v = (const float*)(veh + kMoveSpeed);
    const float* m = *(const float* const*)(veh + kMatrix);
    if (type == 0 && m && (*(const uint32_t*)(veh + kHandlingFlags) & kNitroFlag) && *(const float*)(veh + kTireTemp) < 0.0f) {
        const float dir = v[0] * m[4] + v[1] * m[5] + v[2] * m[6];
        if (dir > 0.2f) return SpeedFxInputRow(fminf(fmaxf(2.0f * dir * (*(const float*)(veh + kGasPedal) + 1.0f), 0.0f), 1.0f));
    }
    if (*g_cutscene) return -1;
    return SpeedFxInputRow(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
}

void PublishFrame(void*, void*, float, float) {
    if (!OnGameThread()) return;
    FxState s;
    s.valid = true;
    s.on = cfg.enabled;
    const uint8_t active = g_camera[kActiveCam];
    const uint32_t look = *(const uint32_t*)(g_camera + kCamLook + kCamStride * active);
    const uint16_t mode = *(const uint16_t*)(g_camera + kCamMode + kCamStride * active);
    s.speedFxRow = SpeedFxRow();
    s.speedFxLook = look == 1 || look == 2 ? (int)look : 0;
    s.radiosity = cfg.radiosity;
    s.radiosityIntensity = cfg.radiosityIntensity;
    s.radiosityOffset = cfg.radiosityOffset;
    s.intensityLimit = *(const int32_t*)(g_colours + kHighLightMinIntensity);
    s.grainStrength = cfg.grain ? cfg.grainStrength : 0.0f;
    s.waterDrops = cfg.waterDrops;
    s.maxDrops = cfg.maxDrops;
    if (const float* m = *(const float* const*)(g_camera + kCamMatrix)) memcpy(s.cam, m, sizeof s.cam);

    // Rain grain: CPostEffects::Render moves s_CurrentStrength one step per frame towards 128 * Rain and draws
    // Grain(strength / 4) outside no-rain zones, above water, outside and below 900 m.
    const float rain = *g_rain, under = *g_underWater;
    const bool noRainZone = ((*g_zoneFlagsP | *g_zoneFlagsC) & 8) != 0;
    static int strength = 0;
    if (rain > 0.0f || strength != 0) {
        if ((float)strength < 128.0f * rain) ++strength;
        else if ((float)strength > 128.0f * rain) --strength;
        if (strength < 0) strength = 0;
        if (!noRainZone && under <= 0.0f && *g_currArea == 0 && s.cam[14] <= 900.0f) s.grainMask = strength / 4;
    }
    // Water drops (skygfx WaterDrops::NoDrops / NoRain / Render): modes 1 and 54 are top-down, 16 MODE_1STPERSON;
    // looking around from a 1st-person car (direction != 3, forward) stops them; the entry/exit fade clears them.
    const bool veh = g_findVehicle(-1, false) != nullptr;
    const bool cutscene = *g_cutscene != 0;
    s.rain = rain;
    s.noDrops = under > 0.339731634f || *g_exitEnterState != 0;
    s.noRain = noRainZone || *g_currArea != 0 || s.noDrops;
    s.firstPerson = mode == 16;
    s.dropsEnabled = mode != 1 && mode != 54 && !(mode == 16 && veh && look != 3);
    s.hideDrops = cutscene || (mode == 16 && !veh);
    PublishFx(s);
}

// ---------------------------------------------------------------- water drop sources (PC tools.cpp)
float CamDist(const float* p) {
    std::lock_guard<std::mutex> l(g_mu);
    const float dx = p[0] - g_camPos[0], dy = p[1] - g_camPos[1], dz = p[2] - g_camPos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// skygfx WaterDrops::RegisterSplash: within 10 m of the camera.
void RegisterSplash(const float* p, int duration) {
    if (cfg.enabled && cfg.waterDrops && p && CamDist(p) <= 10.0f) FxSplash(duration);
}

typedef void (*SplashBig_Fn)(void*, const float*, float);
typedef void (*Splash_Fn)(void*, const float*, float, float);
typedef void (*SplashSmall_Fn)(void*, const float*, bool);
SplashBig_Fn o_SplashBig;
Splash_Fn o_Splash;
SplashSmall_Fn o_SplashSmall;
void H_SplashBig(void* fx, const float* p, float a) { RegisterSplash(p, 1); o_SplashBig(fx, p, a); }
void H_Splash(void* fx, const float* p, float a, float b) { RegisterSplash(p, 1); o_Splash(fx, p, a, b); }
void H_SplashSmall(void* fx, const float* p, bool a) { RegisterSplash(p, 1); o_SplashSmall(fx, p, a); }

// Hydrants and fountains: the water audio event (skygfx: RegisterSplash(point, 20.0f, 20) within 10 m).
typedef void (*AudioEvent_Fn)(void*, int, const float*);
AudioEvent_Fn o_AudioEvent;
void H_AudioEvent(void* self, int event, const float* p) {
    if (event == kWaterAudioEvent) RegisterSplash(p, 20);
    o_AudioEvent(self, event, p);
}

// Boat splash, wake and water splash particles: fill the screen with 2 / dist within 40 / 10 / 30 m (PC tools.cpp).
// FxSystem_c::AddParticle(this, RwV3d* pos, RwV3d* vel, float, FxPrtMult_c*, float, float, float, uint8) (0x4b0b758):
// only this and pos are read; the wrapper hands x0-x7 and d0-d3 back unchanged, which covers every argument.
typedef void* (*AddParticle_Fn)(void*, const float*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                double, double, double, double);
AddParticle_Fn o_AddParticle;
void* H_AddParticle(void* sys, const float* p, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                    double s0, double s1, double s2, double s3) {
    if (cfg.enabled && cfg.waterDrops && p && sys) {
        const float range = sys == *(void**)(g_fxObj + kPrtBoatSplash) ? 40.0f
                          : sys == *(void**)(g_fxObj + kPrtWake) ? 10.0f
                          : sys == *(void**)(g_fxObj + kPrtWaterSplash) ? 30.0f : 0.0f;
        if (range > 0.0f) {
            const float dist = fmaxf(CamDist(p), 0.1f);
            if (dist <= range) FxDropFill(2.0f / dist);
        }
    }
    return o_AddParticle(sys, p, a, b, c, d, e, f, s0, s1, s2, s3);
}

bool HookSym(const char* sym, void* fn, void** orig) {
    void* p = Sym(sym);
    if (!p || !aml->Hook(p, fn, orig)) { SKY_LOG("fxstate: hook failed %s", sym); return false; }
    return true;
}

void Init() {
    g_findVehicle = (FindVehicleFn)Sym("_Z17FindPlayerVehicleib");
    g_camera = (const uint8_t*)Sym("TheCamera");
    g_cutscene = (const uint8_t*)Sym("_ZN12CCutsceneMgr10ms_runningE");
    g_colours = (const uint8_t*)Sym("_ZN10CTimeCycle16m_CurrentColoursE");
    g_rain = (const float*)Sym("_ZN8CWeather4RainE");
    g_underWater = (const float*)Sym("_ZN8CWeather14UnderWaternessE");
    g_currArea = (const int32_t*)Sym("_ZN5CGame8currAreaE");
    g_zoneFlagsP = (const uint32_t*)Sym("_ZN10CCullZones19CurrentFlags_PlayerE");
    g_zoneFlagsC = (const uint32_t*)Sym("_ZN10CCullZones19CurrentFlags_CameraE");
    g_exitEnterState = (const int32_t*)Sym("_ZN17CEntryExitManager17ms_exitEnterStateE");
    g_fxObj = (uint8_t*)Sym("g_fx");
    if (!g_findVehicle || !g_camera || !g_cutscene || !g_colours || !g_rain || !g_underWater || !g_currArea ||
        !g_zoneFlagsP || !g_zoneFlagsC || !g_exitEnterState || !g_fxObj) {
        SKY_LOG("fxstate: symbols missing: post effects off");
        return;
    }
    const bool hooks = HookSym("_ZN4Fx_c18TriggerWaterSplashERK7CVectorf", (void*)&H_SplashBig, (void**)&o_SplashBig) &
                       HookSym("_ZN4Fx_c19TriggerBulletSplashER7CVectorff", (void*)&H_Splash, (void**)&o_Splash) &
                       HookSym("_ZN4Fx_c17TriggerFootSplashER7CVectorb", (void*)&H_SplashSmall, (void**)&o_SplashSmall) &
                       HookSym("_ZN18CAEFireAudioEntity13AddAudioEventEiR7CVector", (void*)&H_AudioEvent, (void**)&o_AudioEvent) &
                       HookSym("_ZN10FxSystem_c11AddParticleEP5RwV3dS1_fP11FxPrtMult_cfffh", (void*)&H_AddParticle,
                               (void**)&o_AddParticle);
    if (!hooks) SKY_LOG("fxstate: some water drop sources are missing");
    Observe(Point::Tick, &PublishFrame);
    SKY_LOG("fxstate: ready");
}

void Status() {
    FxState s;
    if (!LatestFx(&s)) { SKY_LOG("fx: none published"); return; }
    SKY_LOG("fx: speedfx=%d look=%d radiosity=%d limit=%d grain=%d drops=%d rain=%.2f noRain=%d noDrops=%d enabled=%d "
            "first=%d hide=%d", s.speedFxRow, s.speedFxLook, s.radiosity, s.intensityLimit, s.grainMask, s.waterDrops,
            s.rain, s.noRain, s.noDrops, s.dropsEnabled, s.firstPerson, s.hideDrops);
}

const Feature kFeature = {"fxstate", Init, nullptr, false, Status};
const Registrar kRegistrar(kFeature);

}  // namespace

void PublishFx(const FxState& s) {
    std::lock_guard<std::mutex> l(g_mu);
    g_latest = s;
    memcpy(g_camPos, s.cam + 12, sizeof g_camPos);
}

bool LatestFx(FxState* out) {
    std::lock_guard<std::mutex> l(g_mu);
    *out = g_latest;
    return out->valid;
}

}  // namespace skygfx
