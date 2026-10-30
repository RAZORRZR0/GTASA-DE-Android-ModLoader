// Debug weather and clock tools (port map 2.15): forced / blended weather, set time, freeze, speed, timelapse.
// Freecam and noclip are not ported here (camera and pad paths unverified on Android).
#include <stdint.h>
#include <atomic>
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
    if (hour >= 0) {
        g_SetGameClock((uint8_t)hour, (uint8_t)g_setMinute.load(), 0);
    }
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

void ToolsInit() {
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

void ToolsConfigChanged() {
    Pending_Weather();
    g_setHour.store(cfg.setHour);
    g_setMinute.store(cfg.setMinute);
    g_freezeTime.store(cfg.freezeTime);
    g_timeSpeed.store(cfg.timeSpeed);
    g_timelapse.store(cfg.timelapse);
}

}  // namespace

static const Registrar kToolsFeature({"tools", ToolsInit, ToolsConfigChanged});
