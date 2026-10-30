// SkyGfx SpeedFX: port of skygfxde tools.cpp SpeedFxRowUpdate (row from speed) and postfx.cpp SpeedFx (render).
// PC draws the frame `passes` times at alpha 36 with a zoom of shift * 0.25 % of the screen per pass. On Android the
// same row drives UE's PostProcessVolume MotionBlur instead:
//   MotionBlurAmount = 1 - (1 - 36/255)^passes   (the passes composited, as on PC)
//   MotionBlurMax    = passes * shift * 0.25      (percent of screen width, the PC zoom distance per side)
// Look behind (PC look == 1): no effect. Look sideways (PC look == 2): shift halved (integer), as PC.
// shortcut: PC's per-frame wobble has no UE motion-blur equivalent, not ported; the blur is a streak, not a zoom.
// Feature contract: skygfx.h.
#include <math.h>
#include <stdint.h>
#include "skygfx.h"

namespace skygfx {
namespace {

typedef void* (*StaticFn)();
typedef void* (*FindVehicleFn)(int playerNum, bool includeRemote);  // FindPlayerVehicle(int, bool)
typedef int (*LookDirFn)(void* camera);                             // CCamera::GetLookDirection()

// PC kSpeedFx {speed, passes, shift, wobble}; wobble unused (see shortcut above).
struct SpeedFxRow { float speed; int passes, shift; };
constexpr SpeedFxRow kSpeedFx[] = {
    {0.6f, 1, 4}, {0.7f, 2, 4}, {0.8f, 3, 4}, {0.9f, 3, 4}, {0.93f, 4, 4}, {0.96f, 4, 4}, {1.0f, 5, 4},
};
constexpr int kRows = sizeof(kSpeedFx) / sizeof(kSpeedFx[0]);
constexpr float kSpeedFxAlpha = 36.0f;

// arm64 offsets. CEntity matrix pointer (arm64_map O1), CPhysical m_vecMoveSpeed (arm64_map O7).
constexpr uint32_t kMatrix = 0x18, kMoveSpeed = 0x78;
// CVehicle m_nVehicleSubType: PC +0x894.
// shortcut: arm64 offset not confirmed (disassembly scan found no load); verify before trusting the heli/plane/boat/train exclusion.
constexpr uint32_t kSubType = 0x894;
// CAutomobile, verified in NitrousControl 0x57e6ea8: handling flags (u32), tire temperature (float), gas pedal (float).
constexpr uint32_t kHandlingFlags = 0x5C0, kTireTemp = 0xC0C, kGasPedal = 0x710;
constexpr uint32_t kNitroFlag = 0x80000;

struct Offsets {
    uint32_t settings = 0;                  // APostProcessVolume::Settings
    uint32_t amount = 0, max = 0;           // FPostProcessSettings MotionBlurAmount / MotionBlurMax (float)
    uint8_t ovAmountByte = 0, ovAmountMask = 0, ovMaxByte = 0, ovMaxMask = 0;
    bool ok = false;
};
Offsets o;
FindVehicleFn g_findVehicle;
LookDirFn g_lookDir;
void* g_camera;                             // TheCamera (object, exported B symbol)
const uint8_t* g_cutscene;                  // CCutsceneMgr::ms_running (bool, exported B symbol)

// Byte offset + mask of a bool override in FPostProcessSettings (FBoolProperty), as colour.cpp.
bool BoolBit(void* ustruct, const char* name, uint8_t* byte, uint8_t* mask) {
    void* p = FindProperty(ustruct, name);
    if (!p) return false;
    *byte = *((uint8_t*)p + UE::Bool_ByteOffset);
    *mask = *((uint8_t*)p + UE::Bool_FieldMask);
    return true;
}

bool Resolve() {
    StaticFn sstruct = reinterpret_cast<StaticFn>(Sym("_Z12StaticStructI20FPostProcessSettingsEP13UScriptStructv"));
    StaticFn sppv = reinterpret_cast<StaticFn>(Sym("_Z11StaticClassI18APostProcessVolumeEP6UClassv"));
    g_findVehicle = reinterpret_cast<FindVehicleFn>(Sym("_Z17FindPlayerVehicleib"));
    g_lookDir = reinterpret_cast<LookDirFn>(Sym("_ZN7CCamera16GetLookDirectionEv"));
    g_camera = Sym("TheCamera");
    g_cutscene = reinterpret_cast<const uint8_t*>(Sym("_ZN12CCutsceneMgr10ms_runningE"));
    if (!sstruct || !sppv || !g_findVehicle || !g_lookDir || !g_camera || !g_cutscene) return false;
    void* settingsStruct = sstruct();
    void* ppvCls = sppv();
    if (!settingsStruct || !ppvCls) return false;
    bool ok = FieldOffset(ppvCls, "Settings", &o.settings) &&
              FieldOffset(settingsStruct, "MotionBlurAmount", &o.amount) &&
              FieldOffset(settingsStruct, "MotionBlurMax", &o.max) &&
              BoolBit(settingsStruct, "bOverride_MotionBlurAmount", &o.ovAmountByte, &o.ovAmountMask) &&
              BoolBit(settingsStruct, "bOverride_MotionBlurMax", &o.ovMaxByte, &o.ovMaxMask);
    o.ok = ok;
    return ok;
}

// PC SpeedFxInputRow: the last row whose threshold the input reaches, -1 below the first.
int InputRow(float input) {
    for (int i = kRows - 1; i >= 0; --i)
        if (input >= kSpeedFx[i].speed) return i;
    return -1;
}

// PC SpeedFxRowUpdate: row for this frame, -1 for none. Game thread.
int ComputeRow() {
    if (!cfg.enabled || !cfg.speedFx) return -1;
    if (cfg.speedFxTest) return InputRow(1.0f);
    uint8_t* veh = (uint8_t*)g_findVehicle(-1, false);  // ped in a vehicle, local player (PC ped+0x7C8 path)
    if (!veh) return -1;
    const uint32_t type = *(const uint32_t*)(veh + kSubType);
    if (type >= 3 && type <= 6) return -1;             // heli, plane, boat, train
    const float* speed = (const float*)(veh + kMoveSpeed);
    // Nitro: automobile with the nitro handling flag and negative tire temperature; forward speed gates it.
    if (type == 0 && (*(const uint32_t*)(veh + kHandlingFlags) & kNitroFlag) && *(const float*)(veh + kTireTemp) < 0.0f) {
        const void* mat = *(void* const*)(veh + kMatrix);  // CMatrix: forward at float index 4..6
        if (mat) {
            const float* fwd = (const float*)mat + 4;
            const float dir = speed[0] * fwd[0] + speed[1] * fwd[1] + speed[2] * fwd[2];
            if (dir > 0.2f) {
                const float gas = *(const float*)(veh + kGasPedal);
                return InputRow(fminf(fmaxf(2.0f * dir * (gas + 1.0f), 0.0f), 1.0f));
            }
        }
    }
    if (*g_cutscene) return -1;
    return InputRow(sqrtf(speed[0] * speed[0] + speed[1] * speed[1] + speed[2] * speed[2]));
}

// PC look packing: 1 behind, 2 sideways, 0 otherwise.
int LookOf() {
    const int look = g_lookDir(g_camera);
    return look == 1 || look == 2 ? look : 0;
}

// ---- per-volume snapshot of DE's blur values, restored when SpeedFX is off (as colour.cpp OriginalBias).
struct Snap {
    void* obj = nullptr;
    bool applied = false;
    float amount = 0, max = 0;
    bool ovAmount = false, ovMax = false;
};
Snap g_snaps[8];
int g_snapCount;

Snap* SnapFor(void* obj) {
    for (int i = 0; i < g_snapCount; ++i)
        if (g_snaps[i].obj == obj) return &g_snaps[i];
    if (g_snapCount >= 8) return nullptr;
    Snap& v = g_snaps[g_snapCount++];
    v.obj = obj;
    return &v;
}

void SetBit(uint8_t* s, uint8_t byte, uint8_t mask, bool on) {
    if (on) s[byte] |= mask;
    else s[byte] &= (uint8_t)~mask;
}

void Restore(Snap* snap, uint8_t* s) {
    if (!snap->applied) return;
    *(float*)(s + o.amount) = snap->amount;
    *(float*)(s + o.max) = snap->max;
    SetBit(s, o.ovAmountByte, o.ovAmountMask, snap->ovAmount);
    SetBit(s, o.ovMaxByte, o.ovMaxMask, snap->ovMax);
    snap->applied = false;
}

// Writes the blur for this frame (PC SpeedFx render mapped to MotionBlur, see header comment).
void Apply(Snap* snap, uint8_t* s, int row, int look) {
    if (!snap->applied) {
        snap->amount = *(float*)(s + o.amount);
        snap->max = *(float*)(s + o.max);
        snap->ovAmount = (s[o.ovAmountByte] & o.ovAmountMask) != 0;
        snap->ovMax = (s[o.ovMaxByte] & o.ovMaxMask) != 0;
        snap->applied = true;
    }
    const SpeedFxRow& r = kSpeedFx[row];
    const int shift = look == 2 ? r.shift / 2 : r.shift;
    const float alpha = kSpeedFxAlpha / 255.0f;
    *(float*)(s + o.amount) = 1.0f - powf(1.0f - alpha, (float)r.passes);
    *(float*)(s + o.max) = (float)(r.passes * shift) * 0.25f;
    SetBit(s, o.ovAmountByte, o.ovAmountMask, true);
    SetBit(s, o.ovMaxByte, o.ovMaxMask, true);
}

void OnPPUpdate(void* self, void*, float, float) {
    if (!o.ok || !self) return;
    Snap* snap = SnapFor(self);
    if (!snap) return;
    uint8_t* s = (uint8_t*)self + o.settings;
    const int row = ComputeRow();
    const int look = row < 0 ? 0 : LookOf();
    if (row < 0 || look == 1) {  // no SpeedFX, or looking behind: PC draws nothing
        Restore(snap, s);
        return;
    }
    Apply(snap, s, row, look);
}

// Self-check of the row table against PC SpeedFxInputRow boundaries.
bool RowSelfCheck() {
    return InputRow(0.59f) == -1 && InputRow(0.6f) == 0 && InputRow(0.7f) == 1 && InputRow(0.95f) == 4 && InputRow(0.96f) == 5 &&
           InputRow(1.0f) == 6 && InputRow(3.0f) == 6;
}

void Init() {
    if (!Resolve()) {
        SKY_LOG("speedfx: reflection/symbols unresolved, SpeedFX disabled");
        return;
    }
    if (!RowSelfCheck()) {
        SKY_LOG("speedfx: row table self-check failed, SpeedFX disabled");
        return;
    }
    Observe(Point::PPUpdateColorOptions, OnPPUpdate);
    SKY_INFO("speedfx: ready (amount %u, max %u)", o.amount, o.max);
}

const Feature kFeature = {"speedfx", Init, nullptr};
const Registrar kRegistrar(kFeature);

}  // namespace
}  // namespace skygfx
