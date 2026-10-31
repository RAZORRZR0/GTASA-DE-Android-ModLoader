// Post effects drawn on the GPU (vkfx.cpp), as PC SkyGfxDE's postfx.cpp: water drops, SpeedFX, radiosity, grain.
// The game thread publishes one FxState per frame (fxstate.cpp, PC tools.cpp SpeedFxRowUpdate); the render thread that
// records UE's Vulkan commands reads the latest one when it draws. Settings travel inside the state, so the render
// thread never reads cfg (which a reload may be rewriting).
#pragma once
#include <stdint.h>

namespace skygfx {

struct FxState {
    bool valid = false;        // a game frame has published at least once
    bool on = false;           // [General] Enabled
    // SpeedFX: kSpeedFx row 0..6 or -1, and the look (1 behind, 2 sideways, 0 otherwise)
    int speedFxRow = -1, speedFxLook = 0;
    // Radiosity
    bool radiosity = false;
    int radiosityIntensity = 35;
    float radiosityOffset = 0.2f;
    int intensityLimit = 0;    // CColourSet::m_nHighLightMinIntensity (timecyc "IntensityLimit")
    // Grain
    int grainMask = 0;         // PS2 grain alpha mask for this frame, 0 = none
    float grainStrength = 1.0f;
    // Water drops (skygfx WaterDrops inputs)
    bool waterDrops = false;
    int maxDrops = 2000;
    float cam[16] = {};        // camera matrix: rows right, forward, up, position (metres)
    float rain = 0.0f;         // CWeather::Rain
    bool noRain = false, noDrops = false, dropsEnabled = false, firstPerson = false, hideDrops = false;
};

// Game thread: publish this frame's state. Render thread: copy the latest (false before the first publish).
void PublishFx(const FxState& s);
bool LatestFx(FxState* out);

// Water drop sources, any thread (PC tools.cpp RegisterSplash / g_dropFill): a splash burst lasting `frames` drop
// steps, and a fill amount added to the next step.
void FxSplash(int frames);
void FxDropFill(float amount);

}  // namespace skygfx
