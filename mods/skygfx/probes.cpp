// SkyGfx on-device probes (formerly skygfx.cpp). Disabled unless [Debug] Probes=1; no rendering changes.
// Logcat tag: SkyGfx, lines prefixed P1..P6.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <mod/amlmod.h>
#include "skygfx.h"
#include "../../sdk/gtade.h"

using namespace skygfx;

#define LOG SKY_LOG

template <class T> static inline bool Rd(const void* p, T& out) { return p && gtade::SafeRead(p, &out, sizeof(T)); }
static inline const uint8_t* B(const void* p, size_t o) { return (const uint8_t*)p + o; }

typedef void (*ProcessEvent_Fn)(void*, void*, void*);
typedef void (*FNamePlain_Fn)(const void*, char (*)[1024]);
typedef void* (*Getter_Fn)(void);

static ProcessEvent_Fn o_ProcessEvent;
static FNamePlain_Fn g_FNamePlain;
static Getter_Fn g_clsTOD, g_clsFog, g_structPPS;

// Data and function symbols (resolved in ProbesInit; null when missing).
static void *s_maxFL, *s_maxSP, *s_streetDist, *s_streetShadow, *s_useLO, *s_useNNLO, *s_curColours, *s_skyTopRed;
static void *s_pe, *s_plain, *s_clsTOD, *s_clsFog, *s_structPPS;

static double g_t0;
static uint32_t g_p5Claimed;

static double Now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void* CallGetter(Getter_Fn fn) { return fn ? fn() : nullptr; }

static void NameOf(const void* base, size_t off, char* out, size_t cap) {
    UE::FNameRaw n = {0, 0};
    if (!base || !gtade::SafeRead(B(base, off), &n, sizeof n) || n.Index <= 0) { snprintf(out, cap, "?"); return; }
    char buf[1024];
    g_FNamePlain(&n, &buf);
    snprintf(out, cap, "%s", buf);
}

static void ClassNameOf(const void* prop, char* out, size_t cap) {
    void* cc = nullptr;
    if (!Rd(B(prop, UE::Field_Class), cc) || !cc) { snprintf(out, cap, "?"); return; }
    NameOf(cc, 0, out, cap);  // FFieldClass::Name at +0
}

// ---- P1: RHI feature level / shader platform (raw ints) ----
static void P1(const char* tag) {
    int32_t fl = 0, sp = 0;
    uint8_t sp8 = 0;
    if (Rd(s_maxFL, fl)) LOG("%s GMaxRHIFeatureLevel=%d (0x%08X)", tag, fl, (uint32_t)fl);
    else LOG("%s GMaxRHIFeatureLevel unreadable/missing", tag);
    if (Rd(s_maxSP, sp) && Rd(s_maxSP, sp8)) LOG("%s GMaxRHIShaderPlatform=%d (0x%08X, u8 %u)", tag, sp, (uint32_t)sp, sp8);
    else LOG("%s GMaxRHIShaderPlatform unreadable/missing", tag);
}

// ---- P2: lighting override bytes, polled every 500 ms, logged at start and on change ----
static void* P2Thread(void*) {
    if (!s_useLO || !s_useNNLO) { LOG("P2 missing symbol(s)"); return nullptr; }
    uint8_t lo = 0, nn = 0;
    bool first = true;
    for (;;) {
        uint8_t a = 0, b = 0;
        if (!Rd(s_useLO, a) || !Rd(s_useNNLO, b)) LOG("P2 unreadable");
        else if (first || a != lo || b != nn) {
            LOG("P2 s_UseLightingOverrides=0x%02X s_UseNewNewLightingOverrides=0x%02X%s", a, b, first ? " (start)" : " (changed)");
            lo = a;
            nn = b;
            first = false;
        }
        usleep(500000);
    }
}

// ---- P3: street light distances, float and raw hex ----
static void LogFloat(const char* tag, const char* name, const void* p) {
    uint32_t raw = 0;
    float f = 0;
    if (!Rd(p, raw)) { LOG("%s %s unreadable/missing", tag, name); return; }
    memcpy(&f, &raw, sizeof f);
    LOG("%s %s=%g (raw 0x%08X)", tag, name, f, raw);
}

static void P3(const char* tag) {
    LogFloat(tag, "GStreetLightDistance", s_streetDist);
    LogFloat(tag, "GStreetlightShadowDistance", s_streetShadow);
}

// ---- P4: colour tables ----
static void HexLine(const char* tag, const char* name, const uint8_t* buf, size_t n, size_t off) {
    char line[16 * 3 + 1] = "";
    size_t k = n - off < 16 ? n - off : 16;
    for (size_t i = 0; i < k; i++) snprintf(line + i * 3, 4, "%02X ", buf[off + i]);
    LOG("%s %s+0x%02zX: %s", tag, name, off, line);
}

static void P4() {
    uint8_t cc[0xB8];
    if (s_curColours && gtade::SafeRead(s_curColours, cc, sizeof cc))
        for (size_t off = 0; off < sizeof cc; off += 16) HexLine("P4", "m_CurrentColours", cc, sizeof cc, off);
    else LOG("P4 m_CurrentColours unreadable/missing");
    uint8_t sk[16];
    if (s_skyTopRed && gtade::SafeRead(s_skyTopRed, sk, sizeof sk)) HexLine("P4", "m_nSkyTopRed", sk, sizeof sk, 0);
    else LOG("P4 m_nSkyTopRed unreadable/missing");
}

// ---- P5: reflection offsets, sizes, classes ----
static void LogProp(const char* tag, const void* p, const char* label) {
    if (!p) { LOG("P5 %s %s: not found", tag, label); return; }
    char cls[128];
    ClassNameOf(p, cls, sizeof cls);
    uint32_t off = 0, esz = 0;
    Rd(B(p, UE::Prop_Offset), off);
    Rd(B(p, UE::Prop_ElemSize), esz);
    LOG("P5 %s %s off=0x%X size=%u class=%s", tag, label, off, esz, cls);
    if (!strcmp(cls, "BoolProperty")) {
        uint8_t bo = 0, fm = 0;
        Rd(B(p, UE::Bool_ByteOffset), bo);
        Rd(B(p, UE::Bool_FieldMask), fm);
        LOG("P5 %s %s bool byteOffset=0x%02X fieldMask=0x%02X", tag, label, bo, fm);
    }
}

static void DumpChain(const char* tag, void* cls, bool boolsOnly) {
    if (!cls) { LOG("P5 %s class null", tag); return; }
    void* p = nullptr;
    Rd(B(cls, UE::Struct_Props), p);
    for (int guard = 0; p && guard < 4096; ++guard) {
        char nm[256], cn[128];
        NameOf(p, UE::Field_Name, nm, sizeof nm);
        ClassNameOf(p, cn, sizeof cn);
        if (!boolsOnly || !strcmp(cn, "BoolProperty")) LogProp(tag, p, nm);
        void* nx = nullptr;
        Rd(B(p, UE::Prop_LinkNext), nx);
        p = nx;
    }
}

static void ProbeNamed(const char* tag, void* cls, const char* const* names, size_t n) {
    for (size_t i = 0; i < n; i++) LogProp(tag, cls ? FindProperty(cls, names[i]) : nullptr, names[i]);
}

static void RunP5() {
    void* tod = CallGetter(g_clsTOD);
    void* fog = CallGetter(g_clsFog);
    void* pps = CallGetter(g_structPPS);
    LOG("P5 classes AGTATimeOfDay=%p AGTAHeightFog=%p FPostProcessSettings=%p", tod, fog, pps);

    static const char* const kTod[] = {"SkyUpperColor", "SkyLowerColor", "SkylightColor", "FogColor",
                                       "SunColor", "SkyLightIntensity", "CloudyAmount"};
    static const char* const kFog[] = {"bUseGTAValues", "HeightFogComponent", "TimeOfDayActor"};
    static const char* const kPps[] = {"ColorGain", "GrainIntensity", "BloomIntensity"};
    ProbeNamed("TOD", tod, kTod, sizeof kTod / sizeof *kTod);
    ProbeNamed("FOG", fog, kFog, sizeof kFog / sizeof *kFog);
    ProbeNamed("PPS", pps, kPps, sizeof kPps / sizeof *kPps);
    for (const char* n : kPps) {
        char bn[256];
        snprintf(bn, sizeof bn, "bOverride_%s", n);
        LogProp("PPS", pps ? FindProperty(pps, bn) : nullptr, bn);
    }

    DumpChain("TOD", tod, false);
    DumpChain("FOG", fog, false);
    DumpChain("PPS", pps, true);
}

// Runs P5 once: from the first ProcessEvent once the class getters return, else from the 20 s timer.
static void ClaimP5AndRun() {
    if (!__atomic_exchange_n(&g_p5Claimed, 1, __ATOMIC_ACQ_REL)) RunP5();
}

// ---- P6: hook call counts (observers on the core actor hooks) ----
enum { C_ApplyValues, C_ApplySettings, C_Update, C_Tick, C_UpdateColors, C_UpdateColorOptions, C_COUNT };
static const char* const kCN[C_COUNT] = {"AGTATimeOfDay::ApplyValues", "AGTATimeOfDay::ApplySettings",
                                         "AGTATimeOfDay::Update", "AGTATimeOfDay::Tick",
                                         "AGTAHeightFog::UpdateColors", "AGTAPostProcessVolume::UpdateColorOptions"};
static uint32_t g_cnt[C_COUNT];
static void* g_first[C_COUNT];

static inline void Count(int i, void* self) {
    __atomic_fetch_add(&g_cnt[i], 1, __ATOMIC_RELAXED);
    void* zero = nullptr;
    __atomic_compare_exchange_n(&g_first[i], &zero, self, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static void ObsApplyValues(void* self, void*, float, float) { Count(C_ApplyValues, self); }
static void ObsApplySettings(void* self, void*, float, float) { Count(C_ApplySettings, self); }
static void ObsUpdate(void* self, void*, float, float) { Count(C_Update, self); }
static void ObsTick(void* self, void*, float, float) { Count(C_Tick, self); }
static void ObsUpdateColors(void* self, void*, float, float) { Count(C_UpdateColors, self); }
static void ObsUpdateColorOptions(void* self, void*, float, float) { Count(C_UpdateColorOptions, self); }

static void P6Report() {
    for (int i = 0; i < C_COUNT; i++)
        LOG("P6 %s calls=%u firstThis=%p", kCN[i], __atomic_load_n(&g_cnt[i], __ATOMIC_RELAXED), g_first[i]);
}

// ---- P5 trigger hook: first ProcessEvent after the class getters resolve ----
static void Hooked_ProcessEvent(void* obj, void* fn, void* parms) {
    if (!__atomic_load_n(&g_p5Claimed, __ATOMIC_ACQUIRE) && CallGetter(g_clsTOD) && CallGetter(g_clsFog) && CallGetter(g_structPPS))
        ClaimP5AndRun();
    o_ProcessEvent(obj, fn, parms);
}

// Scheduler: P1 start/+20s, P3 start/+60s, P4 +60s, P5 fallback +20s, P6 report every 5 s.
static void* ProbeThread(void*) {
    P1("P1 start");
    P3("P3 start");
    bool d20 = false, d60 = false;
    double next6 = 5;
    for (;;) {
        usleep(250000);
        double t = Now() - g_t0;
        if (!d20 && t >= 20) {
            d20 = true;
            P1("P1 +20s");
            ClaimP5AndRun();
        }
        if (!d60 && t >= 60) {
            d60 = true;
            P3("P3 +60s");
            P4();
        }
        if (t >= next6) {
            next6 += 5;
            P6Report();
        }
    }
}

// Feature init: runs from the core only when [Debug] Probes=1. Hooks install here, so a change needs a restart.
static void ProbesInit() {
    if (!cfg.probes) return;
    g_t0 = Now();

    struct Entry { const char* sym; void** out; };
    const Entry syms[] = {
        {"_ZN7UObject12ProcessEventEP9UFunctionPv", &s_pe},
        {"_ZNK5FName18GetPlainANSIStringERA1024_c", &s_plain},
        {"_ZN13AGTATimeOfDay11StaticClassEv", &s_clsTOD},
        {"_ZN13AGTAHeightFog11StaticClassEv", &s_clsFog},
        {"_ZN20FPostProcessSettings12StaticStructEv", &s_structPPS},
        {"GMaxRHIFeatureLevel", &s_maxFL},
        {"GMaxRHIShaderPlatform", &s_maxSP},
        {"GStreetLightDistance", &s_streetDist},
        {"GStreetlightShadowDistance", &s_streetShadow},
        {"_ZN13AGTATimeOfDay22s_UseLightingOverridesE", &s_useLO},
        {"_ZN13AGTATimeOfDay28s_UseNewNewLightingOverridesE", &s_useNNLO},
        {"_ZN10CTimeCycle16m_CurrentColoursE", &s_curColours},
        {"_ZN10CTimeCycle12m_nSkyTopRedE", &s_skyTopRed},
    };
    for (const Entry& e : syms) *e.out = Sym(e.sym);
    if (!s_pe || !s_plain) { LOG("ABORT: probe ProcessEvent/FName symbol missing"); return; }
    g_FNamePlain = (FNamePlain_Fn)s_plain;
    g_clsTOD = (Getter_Fn)s_clsTOD;
    g_clsFog = (Getter_Fn)s_clsFog;
    g_structPPS = (Getter_Fn)s_structPPS;

    if (!aml->Hook(s_pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent)) { LOG("ABORT: ProcessEvent hook failed"); return; }

    Observe(Point::ApplyValues, ObsApplyValues);
    Observe(Point::ApplySettings, ObsApplySettings);
    Observe(Point::Update, ObsUpdate);
    Observe(Point::Tick, ObsTick);
    Observe(Point::FogUpdateColors, ObsUpdateColors);
    Observe(Point::PPUpdateColorOptions, ObsUpdateColorOptions);

    P1("P1 load");
    pthread_t th1, th2;
    pthread_create(&th1, nullptr, ProbeThread, nullptr);
    pthread_detach(th1);
    pthread_create(&th2, nullptr, P2Thread, nullptr);
    pthread_detach(th2);
    LOG("installed");
}

static const Registrar kProbesFeature({"probes", ProbesInit, nullptr});
