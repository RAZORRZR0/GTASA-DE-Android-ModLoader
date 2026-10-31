// PS2 timecyc table load (port map 2.2). Writes CTimeCycle tables after the game's own Initialise.
// Column order and kinds follow the PC kTimecycLoads map; symbols are exported from libUE4.so.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <vector>
#include <mod/amlmod.h>
#include "skygfx.h"

using namespace skygfx;

namespace {

enum Kind { K_U8, K_X10, K_I16 };
constexpr int kHours = 8, kWeathers = 23, kEntries = kHours * kWeathers; // 184
constexpr int kSlotHours[kHours] = {0, 5, 6, 7, 12, 19, 20, 22};
constexpr int kColumns = 52;

struct Col { const char* sym; Kind kind; }; // sym == nullptr: column is skipped (directional light)

// File column order (0..51). Byte offsets in the file are the column index.
const Col kCols[kColumns] = {
    {"_ZN10CTimeCycle13m_nAmbientRedE", K_U8},
    {"_ZN10CTimeCycle15m_nAmbientGreenE", K_U8},
    {"_ZN10CTimeCycle14m_nAmbientBlueE", K_U8},
    {"_ZN10CTimeCycle17m_nAmbientRed_ObjE", K_U8},
    {"_ZN10CTimeCycle19m_nAmbientGreen_ObjE", K_U8},
    {"_ZN10CTimeCycle18m_nAmbientBlue_ObjE", K_U8},
    {nullptr, K_U8}, {nullptr, K_U8}, {nullptr, K_U8},
    {"_ZN10CTimeCycle12m_nSkyTopRedE", K_U8},
    {"_ZN10CTimeCycle14m_nSkyTopGreenE", K_U8},
    {"_ZN10CTimeCycle13m_nSkyTopBlueE", K_U8},
    {"_ZN10CTimeCycle15m_nSkyBottomRedE", K_U8},
    {"_ZN10CTimeCycle17m_nSkyBottomGreenE", K_U8},
    {"_ZN10CTimeCycle16m_nSkyBottomBlueE", K_U8},
    {"_ZN10CTimeCycle13m_nSunCoreRedE", K_U8},
    {"_ZN10CTimeCycle15m_nSunCoreGreenE", K_U8},
    {"_ZN10CTimeCycle14m_nSunCoreBlueE", K_U8},
    {"_ZN10CTimeCycle15m_nSunCoronaRedE", K_U8},
    {"_ZN10CTimeCycle17m_nSunCoronaGreenE", K_U8},
    {"_ZN10CTimeCycle16m_nSunCoronaBlueE", K_U8},
    {"_ZN10CTimeCycle10m_fSunSizeE", K_X10},
    {"_ZN10CTimeCycle13m_fSpriteSizeE", K_X10},
    {"_ZN10CTimeCycle19m_fSpriteBrightnessE", K_X10},
    {"_ZN10CTimeCycle17m_nShadowStrengthE", K_U8},
    {"_ZN10CTimeCycle22m_nLightShadowStrengthE", K_U8},
    {"_ZN10CTimeCycle21m_nPoleShadowStrengthE", K_U8},
    {"_ZN10CTimeCycle10m_fFarClipE", K_I16},
    {"_ZN10CTimeCycle11m_fFogStartE", K_I16},
    {"_ZN10CTimeCycle27m_fLightsOnGroundBrightnessE", K_X10},
    {"_ZN10CTimeCycle15m_nLowCloudsRedE", K_U8},
    {"_ZN10CTimeCycle17m_nLowCloudsGreenE", K_U8},
    {"_ZN10CTimeCycle16m_nLowCloudsBlueE", K_U8},
    {"_ZN10CTimeCycle24m_nFluffyCloudsBottomRedE", K_U8},
    {"_ZN10CTimeCycle26m_nFluffyCloudsBottomGreenE", K_U8},
    {"_ZN10CTimeCycle25m_nFluffyCloudsBottomBlueE", K_U8},
    {"_ZN10CTimeCycle11m_fWaterRedE", K_U8},
    {"_ZN10CTimeCycle13m_fWaterGreenE", K_U8},
    {"_ZN10CTimeCycle12m_fWaterBlueE", K_U8},
    {"_ZN10CTimeCycle13m_fWaterAlphaE", K_U8},
    {"_ZN10CTimeCycle15m_fPostFx1AlphaE", K_U8},
    {"_ZN10CTimeCycle13m_fPostFx1RedE", K_U8},
    {"_ZN10CTimeCycle15m_fPostFx1GreenE", K_U8},
    {"_ZN10CTimeCycle14m_fPostFx1BlueE", K_U8},
    {"_ZN10CTimeCycle15m_fPostFx2AlphaE", K_U8},
    {"_ZN10CTimeCycle13m_fPostFx2RedE", K_U8},
    {"_ZN10CTimeCycle15m_fPostFx2GreenE", K_U8},
    {"_ZN10CTimeCycle14m_fPostFx2BlueE", K_U8},
    {"_ZN10CTimeCycle13m_fCloudAlphaE", K_U8},
    {"_ZN10CTimeCycle24m_nHighLightMinIntensityE", K_U8},
    {"_ZN10CTimeCycle16m_nWaterFogAlphaE", K_U8},
    {nullptr, K_U8}, // directional multiplier
};

// Game-owned tables, captured before our first write so the stock table can be restored.
uint8_t* g_tbl[kColumns];          // resolved base per column (null = skipped / missing)
std::vector<uint8_t> g_snap[kColumns];
bool g_snapped = false;
bool g_applied = false;
bool g_ready = false;              // all required symbols resolved
std::atomic<int> g_want{-1};       // -1 no change pending, 0 stock, 1 PS2 table

void (*o_Initialise)(bool) = nullptr;
void (*o_InitForRestart)() = nullptr;

size_t Stride(Kind k) { return k == K_I16 ? 2 : 1; }

// Writes entry e (0..183) of column c. Mirrors the PC load: X10 = value*10 rounded, I16 = 2-byte value.
void Store(int c, int e, float v) {
    uint8_t* base = g_tbl[c];
    if (!base) return;
    const Col& col = kCols[c];
    if (col.kind == K_I16) {
        int16_t x = (int16_t)(int)v;
        memcpy(base + (size_t)e * 2, &x, 2);
    } else {
        int x = (col.kind == K_X10) ? (int)(v * 10.0f + 0.5f) : (int)(v + 0.5f);
        if (x < 0) x = 0;
        if (x > 255) x = 255;
        base[e] = (uint8_t)x;
    }
}

// Parses the PS2 file: whitespace-separated numbers, weather-major (23 weathers, each 8 or 24 hour lines in clock
// order), as PC SkyGfxDE reads it. The game's tables are hour-major (entry h * 23 + w).
// Returns false on any missing or malformed data (table left unchanged).
bool LoadFile(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { SKY_LOG("timecyc: cannot open %s", path); return false; }
    std::vector<std::vector<float>> lines;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        if (buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r') continue;
        std::vector<float> row;
        char* p = buf;
        char* end;
        while (true) {
            float v = strtof(p, &end);
            if (end == p) break;
            row.push_back(v);
            p = end;
        }
        if (!row.empty()) lines.push_back(row);
    }
    fclose(f);
    const bool is24 = lines.size() == (size_t)(24 * kWeathers);
    const bool is8 = lines.size() == (size_t)(kHours * kWeathers);
    if (!is24 && !is8) { SKY_LOG("timecyc: %s has %zu lines, expected %d or %d", path, lines.size(), kHours * kWeathers, 24 * kWeathers); return false; }

    const int perWeather = is24 ? 24 : kHours;
    auto lineOf = [&](int w, int h) { return w * perWeather + (is24 ? kSlotHours[h] : h); };
    for (int w = 0; w < kWeathers; ++w)
        for (int h = 0; h < kHours; ++h)
            if ((int)lines[lineOf(w, h)].size() < kColumns) {
                SKY_LOG("timecyc: line %d short (%zu values)", lineOf(w, h), lines[lineOf(w, h)].size());
                return false;
            }
    for (int w = 0; w < kWeathers; ++w)
        for (int h = 0; h < kHours; ++h)
            for (int c = 0; c < kColumns; ++c)
                if (kCols[c].sym) Store(c, h * kWeathers + w, lines[lineOf(w, h)][c]);
    return true;
}

void Apply(int want) {
    if (!g_ready) return;
    if (want == 1) {
        if (!g_snapped) return;
        char path[512];  // [Timecyc] File: relative to the configs folder unless absolute (PC: next to the .asi)
        if (cfg.timecycFile[0] == '/') snprintf(path, sizeof path, "%s", cfg.timecycFile);
        else snprintf(path, sizeof path, "%s/%s", aml->GetConfigPath(), cfg.timecycFile);
        if (!LoadFile(path)) return;
        g_applied = true;
        SKY_LOG("timecyc: %s applied", path);
    } else if (g_applied) {
        for (int c = 0; c < kColumns; ++c)
            if (g_tbl[c]) memcpy(g_tbl[c], g_snap[c].data(), g_snap[c].size());
        g_applied = false;
    }
}

// Captures the stock tables after the game re-initialised them, then applies the current choice.
void AfterInit() {
    if (!g_applied) {
        for (int c = 0; c < kColumns; ++c) {
            if (!g_tbl[c]) continue;
            g_snap[c].assign(g_tbl[c], g_tbl[c] + kEntries * Stride(kCols[c].kind));
        }
        g_snapped = true;
    }
    if (cfg.ps2Timecyc) Apply(1);
    else Apply(0);
}

void Hooked_Initialise(bool b) {
    o_Initialise(b);
    AfterInit();
}

void Hooked_InitForRestart() {
    o_InitForRestart();
    AfterInit();
}

void Observed_Update(void*, void*, float, float) {
    const int want = g_want.exchange(-1);
    if (want < 0) return;
    if (want == 1) Apply(1);
    else Apply(0);
}

void TimecycInit() {
    bool ok = true;
    for (int c = 0; c < kColumns; ++c) {
        if (!kCols[c].sym) { g_tbl[c] = nullptr; continue; }
        g_tbl[c] = (uint8_t*)Sym(kCols[c].sym);
        if (!g_tbl[c]) { SKY_LOG("timecyc: missing %s", kCols[c].sym); ok = false; }
    }
    void* init = Sym("_ZN10CTimeCycle10InitialiseEb");
    void* restart = Sym("_ZN10CTimeCycle14InitForRestartEv");
    if (!init || !restart) { SKY_LOG("timecyc: Initialise/InitForRestart missing"); ok = false; }
    if (!ok) return;
    if (!aml->Hook(init, (void*)&Hooked_Initialise, (void**)&o_Initialise)) { SKY_LOG("timecyc: Initialise hook failed"); return; }
    if (!aml->Hook(restart, (void*)&Hooked_InitForRestart, (void**)&o_InitForRestart)) { SKY_LOG("timecyc: InitForRestart hook failed"); return; }
    g_ready = true;
    g_want.store(cfg.ps2Timecyc ? 1 : 0);
    Observe(Point::Update, Observed_Update);
    SKY_LOG("timecyc: installed");
}

// Runs on the ModMenu thread: only records the request; the game thread applies it on Update.
void TimecycConfigChanged() {
    g_want.store(cfg.ps2Timecyc ? 1 : 0);
}

}  // namespace

static const Registrar kTimecycFeature({"timecyc", TimecycInit, TimecycConfigChanged, true});

bool skygfx::TimecycApplied() { return g_applied; }
