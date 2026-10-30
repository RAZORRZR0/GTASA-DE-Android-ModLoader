// SkyGfx Android core: AML entry, SkyGfx.ini, engine symbol and reflection helpers, actor hook fan-out.
// Feature contract: skygfx.h.
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <pthread.h>
#include <vector>
#include <mod/amlmod.h>
#include "skygfx.h"
#include "../../sdk/gtade.h"

MYMOD(net.gtamod.skygfx, SkyGfx, 0.1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

namespace skygfx {

Config cfg;

namespace {

struct IniKey {
    const char* section;
    const char* name;
    char kind;  // 'b' bool, 'i' int, 'f' float
    size_t offset;
};

#define KEY(sec, name, kind, field) {sec, name, kind, offsetof(Config, field)}
const IniKey kKeys[] = {
    KEY("General", "Enabled", 'b', enabled),
    KEY("General", "Log", 'i', log),
    KEY("ColourFilter", "PS2Filter", 'b', ps2Filter),
    KEY("ColourFilter", "Strength", 'f', filterStrength),
    KEY("ColourFilter", "GameTimecycStrength", 'f', gameTimecycStrength),
    KEY("ColourFilter", "KeepBrightness", 'f', keepBrightness),
    KEY("Grade", "Saturation", 'f', saturation),
    KEY("Grade", "Contrast", 'f', contrast),
    KEY("Atmosphere", "Mode", 'i', atmosphereMode),
    KEY("Atmosphere", "FogDistance", 'f', fogDistance),
    KEY("Atmosphere", "FogOpacity", 'f', fogOpacity),
    KEY("Atmosphere", "Haze", 'f', haze),
    KEY("Atmosphere", "GroundHaze", 'f', groundHaze),
    KEY("Colours", "Sky", 'f', skyColour),
    KEY("Colours", "Fog", 'f', fogColour),
    KEY("Colours", "Ambient", 'f', ambient),
    KEY("Colours", "Sun", 'f', sunColour),
    KEY("Colours", "Clouds", 'f', cloudColour),
    KEY("Colours", "Brightness", 'f', brightness),
    KEY("Colours", "NightExposure", 'f', nightExposure),
    KEY("Shadows", "Darkness", 'f', shadowDarkness),
    KEY("Timecyc", "PS2Timecyc", 'b', ps2Timecyc),
    KEY("Radiosity", "Radiosity", 'b', radiosity),
    KEY("Radiosity", "Intensity", 'i', radiosityIntensity),
    KEY("Radiosity", "DEBloom", 'f', radiosityDeBloom),
    KEY("Grain", "Grain", 'b', grain),
    KEY("Grain", "GrainStrength", 'f', grainStrength),
    KEY("SpeedFX", "SpeedFX", 'b', speedFx),
    KEY("SpeedFX", "TestMode", 'b', speedFxTest),
    KEY("StreetLights", "DrawDistanceDm", 'i', streetLightDrawDm),
    KEY("StreetLights", "ShadowDistanceDm", 'i', streetLightShadowDm),
    KEY("StreetLights", "OtherLightShadows", 'b', otherLightShadows),
    KEY("StreetLights", "BollardBrightness", 'f', bollardBrightness),
    KEY("StreetLights", "GlobeLampBrightness", 'i', globeLampBrightness),
    KEY("Corona", "Enabled", 'b', corona),
    KEY("Corona", "Size", 'f', coronaSize),
    KEY("Corona", "Intensity", 'f', coronaIntensity),
    KEY("Corona", "FarClipDm", 'i', coronaFarClipDm),
    KEY("World", "LodDistance", 'f', lodDistance),
    KEY("ModernLook", "SSGI", 'b', ssgi),
    KEY("ModernLook", "SSGIQuality", 'i', ssgiQuality),
    KEY("ModernLook", "AmbientOcclusion", 'b', ambientOcclusion),
    KEY("ModernLook", "Reflections", 'b', reflections),
    KEY("ModernLook", "ReflectionQuality", 'i', reflectionQuality),
    KEY("ModernLook", "VolumetricFog", 'b', volumetricFog),
    KEY("ModernLook", "LensFlare", 'b', lensFlare),
    KEY("ModernLook", "SunStrength", 'f', sunStrength),
    KEY("Debug", "ForceWeather", 'i', forceWeather),
    KEY("Debug", "BlendWeatherB", 'i', blendWeatherB),
    KEY("Debug", "Blend", 'f', blend),
    KEY("Debug", "SetHour", 'i', setHour),
    KEY("Debug", "SetMinute", 'i', setMinute),
    KEY("Debug", "FreezeTime", 'b', freezeTime),
    KEY("Debug", "TimeSpeed", 'f', timeSpeed),
    KEY("Debug", "Timelapse", 'b', timelapse),
    KEY("Debug", "Noclip", 'b', noclip),
    KEY("Debug", "Freecam", 'b', freecam),
    KEY("Debug", "NoclipSpeed", 'i', noclipSpeed),
    KEY("Debug", "FreecamSpeed", 'i', freecamSpeed),
    KEY("Debug", "MovePlayerToCamera", 'b', movePlayerToCamera),
    KEY("Debug", "Probes", 'b', probes),
};
#undef KEY

void* g_ue4;
bool g_ready;
void* g_maxFL;     // GMaxRHIFeatureLevel
void* g_tod;       // AGTATimeOfDay*
void* g_fog;       // AGTAHeightFog*
void* g_pp;        // AGTAPostProcessVolume*
pthread_t g_gameThread;
bool g_hasGameThread;

typedef void (*FNameCtor_Fn)(void*, const char*, int);
typedef void* (*FindProp_Fn)(const void*, uint64_t);
FNameCtor_Fn g_nameCtor;
FindProp_Fn g_findProp;

constexpr int kObserversPerPoint = 8;
ObserveFn g_observers[static_cast<int>(Point::kCount)][kObserversPerPoint];
int g_observerCount[static_cast<int>(Point::kCount)];
ObserveFn g_preObservers[static_cast<int>(Point::kCount)][kObserversPerPoint];
int g_preObserverCount[static_cast<int>(Point::kCount)];

std::vector<Feature>& Features() {
    static std::vector<Feature> features;
    return features;
}

void Fan(Point point, void* self, void* arg, float a, float b) {
    const int i = static_cast<int>(point);
    for (int k = 0; k < g_observerCount[i]; k++) g_observers[i][k](self, arg, a, b);
}

void FanPre(Point point, void* self, void* arg, float a, float b) {
    const int i = static_cast<int>(point);
    for (int k = 0; k < g_preObserverCount[i]; k++) g_preObservers[i][k](self, arg, a, b);
}

// Hook trampolines. Each stores the actor first so observers see it, then calls the original, then fans out.
void (*o_ApplyValues)(void*);
void H_ApplyValues(void* self) {
    g_tod = self;
    FanPre(Point::ApplyValues, self, nullptr, 0, 0);
    o_ApplyValues(self);
    Fan(Point::ApplyValues, self, nullptr, 0, 0);
}

void (*o_ApplySettings)(void*, void*, float, float);
void H_ApplySettings(void* self, void* settings, float a, float b) {
    g_tod = self;
    FanPre(Point::ApplySettings, self, settings, a, b);
    o_ApplySettings(self, settings, a, b);
    Fan(Point::ApplySettings, self, settings, a, b);
}

void (*o_Update)(void*, float);
void H_Update(void* self, float dt) {
    g_tod = self;
    FanPre(Point::Update, self, nullptr, dt, 0);
    o_Update(self, dt);
    Fan(Point::Update, self, nullptr, dt, 0);
}

void (*o_Tick)(void*, float);
void H_Tick(void* self, float dt) {
    static bool lateDone;
    if (!lateDone) {  // first game-thread tick: the UObject system is up
        lateDone = true;
        for (const Feature& f : Features()) {
            if (f.early) continue;
            SKY_INFO("init %s", f.name);
            if (f.init) f.init();
            if (f.configChanged) f.configChanged();
        }
    }
    g_tod = self;
    if (!g_hasGameThread) {
        g_gameThread = pthread_self();
        __atomic_store_n(&g_hasGameThread, true, __ATOMIC_RELEASE);
    }
    FanPre(Point::Tick, self, nullptr, dt, 0);
    o_Tick(self, dt);
    Fan(Point::Tick, self, nullptr, dt, 0);
}

void (*o_FogUpdateColors)(void*, float);
void H_FogUpdateColors(void* self, float dt) {
    g_fog = self;
    FanPre(Point::FogUpdateColors, self, nullptr, dt, 0);
    o_FogUpdateColors(self, dt);
    Fan(Point::FogUpdateColors, self, nullptr, dt, 0);
}

void (*o_PPUpdateColorOptions)(void*);
void H_PPUpdateColorOptions(void* self) {
    g_pp = self;
    FanPre(Point::PPUpdateColorOptions, self, nullptr, 0, 0);
    o_PPUpdateColorOptions(self);
    Fan(Point::PPUpdateColorOptions, self, nullptr, 0, 0);
}

char* Trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

void SetValue(Config& out, const IniKey& key, const char* value) {
    char* p = reinterpret_cast<char*>(&out) + key.offset;
    switch (key.kind) {
        case 'b': *reinterpret_cast<bool*>(p) = atoi(value) != 0; break;
        case 'i': *reinterpret_cast<int*>(p) = atoi(value); break;
        case 'f': *reinterpret_cast<float*>(p) = strtof(value, nullptr); break;
    }
}

// Reads path over the defaults. Unknown keys and sections are ignored.
void ReadIni(const char* path, Config& out) {
    FILE* f = fopen(path, "r");
    if (!f) {
        SKY_LOG("%s not found, using defaults", path);
        return;
    }
    char line[256];
    char section[64] = "";
    while (fgets(line, sizeof line, f)) {
        char* s = Trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) {
                *end = 0;
                snprintf(section, sizeof section, "%s", s + 1);
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = Trim(s);
        const char* value = Trim(eq + 1);
        for (const IniKey& k : kKeys) {
            if (!strcasecmp(k.section, section) && !strcasecmp(k.name, key)) {
                SetValue(out, k, value);
                break;
            }
        }
    }
    fclose(f);
}

void LoadConfig() {
    const char* dir = aml->GetConfigPath();
    const size_t n = strlen(dir);
    char path[512];
    snprintf(path, sizeof path, "%s%sSkyGfx.ini", dir, (n && dir[n - 1] == '/') ? "" : "/");
    Config next;
    ReadIni(path, next);
    cfg = next;
}

}  // namespace

void* Sym(const char* name) {
    void* p = g_ue4 ? reinterpret_cast<void*>(aml->GetSym(g_ue4, name)) : nullptr;
    if (!p) SKY_LOG("MISSING %s", name);
    return p;
}

void* FindProperty(void* ustruct, const char* name) {
    if (!ustruct || !g_nameCtor || !g_findProp) return nullptr;
    UE::FNameRaw n = {0, 0};
    g_nameCtor(&n, name, 0);  // FNAME_Find: a name the engine never created yields NAME_None.
    if (n.Index <= 0) return nullptr;
    uint64_t key;
    memcpy(&key, &n, sizeof key);
    return g_findProp(ustruct, key);
}

bool FieldOffset(void* ustruct, const char* name, uint32_t* outOffset) {
    void* prop = FindProperty(ustruct, name);
    return prop && gtade::SafeRead(reinterpret_cast<const uint8_t*>(prop) + UE::Prop_Offset, outOffset, sizeof *outOffset);
}

void* TimeOfDay() { return g_tod; }
void* HeightFog() { return g_fog; }
void* PostProcessVolume() { return g_pp; }

int RhiFeatureLevel() {
    int32_t level = -1;
    if (!g_maxFL || !gtade::SafeRead(g_maxFL, &level, sizeof level)) return -1;
    return level;
}

bool OnGameThread() {
    return __atomic_load_n(&g_hasGameThread, __ATOMIC_ACQUIRE) && pthread_equal(pthread_self(), g_gameThread);
}

void Observe(Point point, ObserveFn fn) {
    const int i = static_cast<int>(point);
    if (g_observerCount[i] >= kObserversPerPoint) {
        SKY_LOG("observer table full for point %d, dropping observer", i);
        return;
    }
    g_observers[i][g_observerCount[i]++] = fn;
}

void ObservePre(Point point, ObserveFn fn) {
    const int i = static_cast<int>(point);
    if (g_preObserverCount[i] >= kObserversPerPoint) {
        SKY_LOG("observer table full for point %d, dropping observer", i);
        return;
    }
    g_preObservers[i][g_preObserverCount[i]++] = fn;
}

Registrar::Registrar(const Feature& feature) { Features().push_back(feature); }

}  // namespace skygfx

using namespace skygfx;

ON_MOD_LOAD() {
    g_ue4 = aml->GetLibHandle("libUE4.so");
    if (!g_ue4) {
        SKY_LOG("ABORT: libUE4.so not loaded");
        return;
    }
    LoadConfig();
    SKY_INFO("config loaded, log=%d", cfg.log);

    g_maxFL = Sym("GMaxRHIFeatureLevel");
    g_nameCtor = reinterpret_cast<FNameCtor_Fn>(Sym("_ZN5FNameC1EPKc9EFindName"));
    g_findProp = reinterpret_cast<FindProp_Fn>(Sym("_ZNK7UStruct18FindPropertyByNameE5FName"));
    if (!g_maxFL || !g_nameCtor || !g_findProp) {
        SKY_LOG("ABORT: core symbol missing");
        return;
    }

    for (const Feature& f : Features()) {  // early features only; the rest wait for the first Tick
        if (!f.early) continue;
        SKY_INFO("init %s (early)", f.name);
        if (f.init) f.init();
    }

    const struct {
        const char* sym;
        void* fn;
        void** orig;
    } hooks[] = {
        {"_ZN13AGTATimeOfDay11ApplyValuesEv", reinterpret_cast<void*>(&H_ApplyValues), reinterpret_cast<void**>(&o_ApplyValues)},
        {"_ZN13AGTATimeOfDay13ApplySettingsER12FSkySettingsff", reinterpret_cast<void*>(&H_ApplySettings), reinterpret_cast<void**>(&o_ApplySettings)},
        {"_ZN13AGTATimeOfDay6UpdateEf", reinterpret_cast<void*>(&H_Update), reinterpret_cast<void**>(&o_Update)},
        {"_ZN13AGTATimeOfDay4TickEf", reinterpret_cast<void*>(&H_Tick), reinterpret_cast<void**>(&o_Tick)},
        {"_ZN13AGTAHeightFog12UpdateColorsEf", reinterpret_cast<void*>(&H_FogUpdateColors), reinterpret_cast<void**>(&o_FogUpdateColors)},
        {"_ZN21AGTAPostProcessVolume18UpdateColorOptionsEv", reinterpret_cast<void*>(&H_PPUpdateColorOptions), reinterpret_cast<void**>(&o_PPUpdateColorOptions)},
    };
    for (const auto& h : hooks) {
        void* addr = Sym(h.sym);
        if (addr && !aml->Hook(addr, h.fn, h.orig)) SKY_LOG("hook failed %s", h.sym);
    }

    g_ready = true;
    for (const Feature& f : Features())
        if (f.early && f.configChanged) f.configChanged();
    SKY_LOG("installed %d features", (int)Features().size());
}

extern "C" void GTAML_OnConfigChanged() {
    if (!g_ready) return;
    LoadConfig();
    for (const Feature& f : Features())
        if (f.configChanged) f.configChanged();
}

// ---- Console variables -------------------------------------------------------------
namespace {
typedef void* (*FindCVar_Fn)(void* mgr, const char16_t* name, bool trackFrequentCalls);
constexpr int kSlotSet = 17, kSlotGetInt = 19, kSlotGetFloat = 20;
constexpr uint32_t kSetByCode = 0x08000000;

void** VTable(void* obj) { return *reinterpret_cast<void***>(obj); }

bool SetCVarText(const char* name, const char* text) {
    void* cv = skygfx::FindCVar(name);
    if (!cv) return false;
    char16_t w[64] = {};
    for (size_t i = 0; text[i] && i < 63; ++i) w[i] = (char16_t)(unsigned char)text[i];
    reinterpret_cast<void (*)(void*, const char16_t*, uint32_t)>(VTable(cv)[kSlotSet])(cv, w, kSetByCode);
    return true;
}
}  // namespace

namespace skygfx {
void* FindCVar(const char* name) {
    static FindCVar_Fn find = (FindCVar_Fn)Sym("_ZNK15FConsoleManager19FindConsoleVariableEPKDsb");
    static void** singleton = (void**)Sym("_ZN15IConsoleManager9SingletonE");
    if (!find || !singleton || !*singleton) return nullptr;
    char16_t w[128] = {};
    for (size_t i = 0; name[i] && i < 127; ++i) w[i] = (char16_t)(unsigned char)name[i];
    return find(*singleton, w, false);
}
bool SetCVarInt(const char* name, int value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    return SetCVarText(name, buf);
}
bool SetCVarFloat(const char* name, float value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%g", value);
    return SetCVarText(name, buf);
}
int GetCVarInt(const char* name, int def) {
    void* cv = FindCVar(name);
    return cv ? reinterpret_cast<int (*)(void*)>(VTable(cv)[kSlotGetInt])(cv) : def;
}
float GetCVarFloat(const char* name, float def) {
    void* cv = FindCVar(name);
    return cv ? reinterpret_cast<float (*)(void*)>(VTable(cv)[kSlotGetFloat])(cv) : def;
}
}  // namespace skygfx
