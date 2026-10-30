// ModMenu: adds a "mods" entry to the pause/main menu templates and logs what fires when it is pressed.
// Step A of docs/mods_menu_design.md (entry injection + event discovery). Logcat tag: ModMenu.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <mod/amlmod.h>
#include "../../sdk/gtade.h"

MYMOD(net.gtamod.modmenu, ModMenu, 0.1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ModMenu", __VA_ARGS__)

template <class T> static inline bool Rd(const void* p, T& out) { return gtade::SafeRead(p, &out, sizeof(T)); }

namespace UE {  // docs/reflection_layout.md
constexpr size_t Obj_Class = 0x10, Obj_Name = 0x18, Struct_Super = 0x40;
constexpr size_t Prop_ElemSize = 0x38, Prop_Offset = 0x4C, Prop_Inner = 0x78;
}

struct FString { char16_t* data; int32_t num, max; };  // num includes the NUL
struct TArrayRaw { uint8_t* data; int32_t num, max; };
struct FTextRaw { uint8_t b[24]; };                    // >16 bytes: returned via x8 on AArch64
struct FNameRaw { int32_t Index, Number; };

typedef void (*ProcessEvent_Fn)(void*, void*, void*);
typedef void (*FNameCtor_Fn)(void*, const char*, int);
typedef void (*FNamePlain_Fn)(const void*, char (*)[1024]);
typedef void* (*FindProp_Fn)(const void*, uint64_t);
typedef const FString* (*TextToString_Fn)(const void*);
typedef FTextRaw (*TextFromString_Fn)(const FString&);
typedef void* (*Malloc_Fn)(size_t, uint32_t);
typedef void* (*Realloc_Fn)(void*, size_t, uint32_t);

static ProcessEvent_Fn o_ProcessEvent;
static FNameCtor_Fn g_FNameCtor;
static FNamePlain_Fn g_FNamePlain;
static FindProp_Fn g_findProp;
static TextToString_Fn g_textToString;
static TextFromString_Fn g_textFromString;
static Malloc_Fn g_malloc;
static Realloc_Fn g_realloc;

enum NameId { N_SetTemplate, N_InteractedButton, N_CustomStr, N_CustomStrText, N_CustomBoolRet, N_COUNT };
static const char* const kNames[N_COUNT] = {"SetTemplate", "InteractedButton", "CustomFunctionFromString",
                                            "CustomFunctionFromStringWithText", "CustomFunctionWithBoolReturn"};
static FNameRaw g_name[N_COUNT];

static const char kModsLabel[] = "mods";
static const char kModsFunction[] = "GTAML_MODS";

static void NameStr(const void* obj, char* out, size_t cap) {
    FNameRaw n = {0, 0};
    if (!obj || !Rd((const uint8_t*)obj + UE::Obj_Name, n) || n.Index <= 0) { snprintf(out, cap, "?"); return; }
    char buf[1024];
    g_FNamePlain(&n, &buf);
    snprintf(out, cap, "%s", buf);
}

static void* ClassOf(const void* obj) {
    void* c = nullptr;
    return Rd((const uint8_t*)obj + UE::Obj_Class, c) ? c : nullptr;
}

static bool Is(int32_t idx, int id) { return idx > 0 && __atomic_load_n(&g_name[id].Index, __ATOMIC_ACQUIRE) == idx; }

static uint8_t* FindProp(void* obj, const char* name) {
    FNameRaw n = {0, 0};
    g_FNameCtor(&n, name, 0);
    if (n.Index <= 0) return nullptr;
    uint64_t key;
    memcpy(&key, &n, sizeof key);
    for (void* s = ClassOf(obj); s; ) {
        if (void* p = g_findProp(s, key)) return (uint8_t*)p;
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
    return nullptr;
}

// TArray property on obj plus its element size, or null.
static TArrayRaw* ArrayProp(void* obj, const char* name, int32_t* elem) {
    uint8_t* p = FindProp(obj, name);
    int32_t off = 0;
    uint8_t* inner = nullptr;
    if (!p || !Rd(p + UE::Prop_Offset, off) || !Rd(p + UE::Prop_Inner, inner) || !inner || !Rd(inner + UE::Prop_ElemSize, *elem))
        return nullptr;
    return (TArrayRaw*)((uint8_t*)obj + off);
}

static void Ascii(const FString* s, char* out, size_t cap) {
    size_t i = 0;
    if (s && s->data)
        for (; i + 1 < cap && (int32_t)i < s->num && s->data[i]; ++i) out[i] = s->data[i] < 128 ? (char)s->data[i] : '?';
    out[i] = 0;
}

static FString MakeString(const char* a) {  // heap FString owned by whoever stores it
    int32_t n = (int32_t)strlen(a) + 1;
    FString s = {(char16_t*)g_malloc(n * sizeof(char16_t), 0), n, n};
    for (int32_t i = 0; i < n; ++i) s.data[i] = (char16_t)a[i];
    return s;
}

// Opens a slot of elem bytes at index ins (tail shifted; UE containers are bitwise-relocatable).
static uint8_t* InsertSlot(TArrayRaw* a, int32_t elem, int32_t ins) {
    if (a->num + 1 > a->max) {
        a->data = (uint8_t*)g_realloc(a->data, (size_t)(a->num + 1) * elem, 0);
        a->max = a->num + 1;
    }
    memmove(a->data + (size_t)(ins + 1) * elem, a->data + (size_t)ins * elem, (size_t)(a->num - ins) * elem);
    ++a->num;
    return a->data + (size_t)ins * elem;
}

// Template assets are shared and outlive menus: patch each one once.
static void* g_patched[8];

static void PatchTemplate(void* tmpl) {
    for (void* p : g_patched) if (p == tmpl) return;
    char tn[128];
    NameStr(tmpl, tn, sizeof tn);
    if (strcmp(tn, "GTASA_PauseMenu_Mobile") != 0 && strcmp(tn, "GTASA_MainMenu_Mobile") != 0) return;
    for (void*& p : g_patched) if (!p) { p = tmpl; break; }

    int32_t eLabels = 0, eFuncs = 0, eTabs = 0;
    TArrayRaw* labels = ArrayProp(tmpl, "LeftButtons", &eLabels);
    TArrayRaw* funcs = ArrayProp(tmpl, "FunctionToCallOnPress", &eFuncs);
    TArrayRaw* tabs = ArrayProp(tmpl, "SelectorTabs", &eTabs);
    if (!labels || !funcs || !tabs || eLabels != 24 || eFuncs != 16 || eTabs != 8) {
        LOG("%s: arrays not found (elem %d/%d/%d), not patched", tn, eLabels, eFuncs, eTabs);
        return;
    }
    const int32_t n = labels->num;
    if (n < 2 || funcs->num != n || tabs->num != n) {
        LOG("%s: array counts differ (%d/%d/%d), not patched", tn, n, funcs->num, tabs->num);
        return;
    }
    int32_t opt = -1;
    for (int32_t i = 0; i < n; ++i) {
        char s[64];
        Ascii(g_textToString(labels->data + i * 24), s, sizeof s);
        if (strcasecmp(s, "options") == 0) opt = i;
        if (strcasecmp(s, kModsLabel) == 0) return;  // already present
    }
    if (opt < 0) { LOG("%s: no options entry, not patched", tn); return; }
    const int32_t ins = n - 1;  // before exit

    // Other parallel arrays of the same length: duplicate the options element (plain data only).
    static const char* const kOthers[] = {"AltLeftButtonText", "LeftButtonSelectedBackgroundOverride"};
    for (const char* name : kOthers) {
        int32_t e = 0;
        TArrayRaw* a = ArrayProp(tmpl, name, &e);
        if (!a || a->num != n) continue;
        if (e == 24 || e == 16) { LOG("%s: %s has text elements, left unchanged", tn, name); continue; }
        uint8_t tmp[64];
        if (e > (int32_t)sizeof tmp) continue;
        memcpy(tmp, a->data + (size_t)opt * e, e);
        memcpy(InsertSlot(a, e, ins), tmp, e);
    }

    FString label = MakeString(kModsLabel);
    FTextRaw text = g_textFromString(label);  // const& overload copies; label stays ours (leaked once, 10 bytes)
    memcpy(InsertSlot(labels, 24, ins), &text, sizeof text);
    FString fn = MakeString(kModsFunction);
    memcpy(InsertSlot(funcs, 16, ins), &fn, sizeof fn);
    void* optTab = *(void**)(tabs->data + (size_t)opt * 8);
    memcpy(InsertSlot(tabs, 8, ins), &optTab, 8);
    LOG("%s: inserted '%s' at %d (tab class copied from index %d)", tn, kModsLabel, ins, opt);
}

static int32_t g_seen[512];  // UI_PauseMenu_C function names already logged
static int g_seenCount;

static void Observe(void* obj, void* func, void* parms) {
    int32_t fn = 0;
    if (!Rd((uint8_t*)func + UE::Obj_Name, fn)) return;

    if (Is(fn, N_SetTemplate)) {
        void* tmpl = nullptr;
        if (parms && Rd(parms, tmpl) && tmpl) PatchTemplate(tmpl);
        return;
    }
    if (Is(fn, N_InteractedButton)) {
        int32_t v[2] = {0, 0};
        if (parms) Rd(parms, v);
        char cn[128];
        NameStr(ClassOf(obj), cn, sizeof cn);
        LOG("InteractedButton on %s: type=%d index=%d", cn, v[0], v[1]);
        return;
    }
    if (Is(fn, N_CustomStr) || Is(fn, N_CustomStrText) || Is(fn, N_CustomBoolRet)) {
        FString s = {};
        char a[128] = "";
        if (parms && Rd(parms, s)) {
            char16_t buf[128] = {};
            int32_t len = s.num > 0 && s.num < 128 ? s.num : 0;
            if (len && gtade::SafeRead(s.data, buf, len * sizeof(char16_t))) {
                FString c = {buf, len, len};
                Ascii(&c, a, sizeof a);
            }
        }
        char fname[96];
        NameStr(func, fname, sizeof fname);
        static char last[128];  // these fire per frame; log changes only
        if (strcmp(last, a) == 0) return;
        snprintf(last, sizeof last, "%s", a);
        LOG("%s(\"%s\")", fname, a);
        return;
    }
    if (g_seenCount < (int)(sizeof g_seen / sizeof g_seen[0])) {
        char cn[128];
        NameStr(ClassOf(obj), cn, sizeof cn);
        if (strcmp(cn, "UI_PauseMenu_C") != 0) return;
        for (int i = 0; i < g_seenCount; ++i) if (g_seen[i] == fn) return;
        g_seen[g_seenCount++] = fn;
        char fname[128];
        NameStr(func, fname, sizeof fname);
        LOG("UI_PauseMenu_C::%s", fname);
    }
}

static void Hooked_ProcessEvent(void* obj, void* func, void* parms) {
    if (obj && func) Observe(obj, func, parms);
    o_ProcessEvent(obj, func, parms);
}

static void* ResolveThread(void*) {
    for (int tries = 0; tries < 600; ++tries) {
        bool all = true;
        for (int i = 0; i < N_COUNT; ++i) {
            if (g_name[i].Index > 0) continue;
            FNameRaw n = {0, 0};
            g_FNameCtor(&n, kNames[i], 0);
            if (n.Index > 0) {
                g_name[i].Number = n.Number;
                __atomic_store_n(&g_name[i].Index, n.Index, __ATOMIC_RELEASE);
            } else {
                all = false;
            }
        }
        if (all) { LOG("names resolved"); return nullptr; }
        usleep(500000);
    }
    LOG("some names unresolved");
    return nullptr;
}

ON_MOD_LOAD() {
    void* h = (void*)aml->GetLibHandle("libUE4.so");
    if (!h) { LOG("ABORT: libUE4.so not loaded"); return; }
    void *pe, *ctor, *plain, *find, *toStr, *fromStr, *mal, *real;
    struct Sym { const char* sym; void** out; } syms[] = {
        {"_ZN7UObject12ProcessEventEP9UFunctionPv", &pe},
        {"_ZN5FNameC1EPKc9EFindName", &ctor},
        {"_ZNK5FName18GetPlainANSIStringERA1024_c", &plain},
        {"_ZNK7UStruct18FindPropertyByNameE5FName", &find},
        {"_ZNK5FText8ToStringEv", &toStr},
        {"_ZN5FText10FromStringERK7FString", &fromStr},
        {"_ZN7FMemory6MallocEyj", &mal},
        {"_ZN7FMemory7ReallocEPvyj", &real},
    };
    for (Sym& s : syms)
        if (!(*s.out = (void*)aml->GetSym(h, s.sym))) { LOG("ABORT: missing %s", s.sym); return; }
    g_FNameCtor = (FNameCtor_Fn)ctor;
    g_FNamePlain = (FNamePlain_Fn)plain;
    g_findProp = (FindProp_Fn)find;
    g_textToString = (TextToString_Fn)toStr;
    g_textFromString = (TextFromString_Fn)fromStr;
    g_malloc = (Malloc_Fn)mal;
    g_realloc = (Realloc_Fn)real;
    if (!aml->Hook(pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent)) { LOG("ABORT: hook failed"); return; }
    pthread_t th;
    pthread_create(&th, nullptr, ResolveThread, nullptr);
    pthread_detach(th);
    LOG("installed");
}
