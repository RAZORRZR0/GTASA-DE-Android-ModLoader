// ModMenu: adds a "mods" entry to the pause/main menu: a list of installed mods (on/off), and per mod
// a page with its ini settings. Rows are created with the game's own widgets (AddSlotItem).
// Logcat tag: ModMenu.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <android/log.h>
#include <mod/amlmod.h>
#include "../../sdk/gtade.h"
#include "modmenu_core.h"

MYMOD(net.gtamod.modmenu, ModMenu, 0.1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ModMenu", __VA_ARGS__)

template <class T> static inline bool Rd(const void* p, T& out) { return gtade::SafeRead(p, &out, sizeof(T)); }

namespace UE {  // docs/reflection_layout.md
constexpr size_t Obj_Class = 0x10, Obj_Name = 0x18, Obj_Outer = 0x20, Struct_Super = 0x40;
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
typedef void* (*FindFn_Fn)(void*, uint64_t, int);
typedef const FString* (*TextToString_Fn)(const void*);
typedef FTextRaw (*TextFromString_Fn)(const FString&);
typedef void* (*Malloc_Fn)(size_t, uint32_t);
typedef void* (*Realloc_Fn)(void*, size_t, uint32_t);

static ProcessEvent_Fn o_ProcessEvent;
static FNameCtor_Fn g_FNameCtor;
static FNamePlain_Fn g_FNamePlain;
static FindProp_Fn g_findProp;
static FindFn_Fn g_findFn;
static TextToString_Fn g_textToString;
static TextFromString_Fn g_textFromString;
static Malloc_Fn g_malloc;
static Realloc_Fn g_realloc;

enum NameId { N_SetTemplate, N_InteractedButton, N_Construct, N_Destruct, N_FinalCheck, N_Tick, N_OnSlotClicked,
              N_UpdateSettings, N_NextSel, N_PrevSel, N_InteractedSel, N_COUNT };
static const char* const kNames[N_COUNT] = {"SetTemplate", "InteractedButton", "Construct", "Destruct",
                                            "On Option Changed Final Check", "Tick", "OnSlotClicked",
                                            "UpdateSettings", "NextSelection", "PreviousSelection",
                                            "InteractedWithSelectionSlot"};
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

static bool ClassIs(const void* obj, const char* name) {
    char cn[128];
    NameStr(ClassOf(obj), cn, sizeof cn);
    return strcmp(cn, name) == 0;
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

// Template assets are shared and outlive menus: patch each one once. modsIndex is the "mods" slot, or -1.
struct PatchedTemplate { void* tmpl; int32_t modsIndex; };
static PatchedTemplate g_patched[8];

static int32_t PatchTemplate(void* tmpl) {
    for (const PatchedTemplate& p : g_patched) if (p.tmpl == tmpl) return p.modsIndex;
    char tn[128];
    NameStr(tmpl, tn, sizeof tn);
    if (strcmp(tn, "GTASA_PauseMenu_Mobile") != 0 && strcmp(tn, "GTASA_MainMenu_Mobile") != 0) return -1;
    PatchedTemplate* slot = nullptr;
    for (PatchedTemplate& p : g_patched) if (!p.tmpl) { p = {tmpl, -1}; slot = &p; break; }

    int32_t eLabels = 0, eFuncs = 0, eTabs = 0;
    TArrayRaw* labels = ArrayProp(tmpl, "LeftButtons", &eLabels);
    TArrayRaw* funcs = ArrayProp(tmpl, "FunctionToCallOnPress", &eFuncs);
    TArrayRaw* tabs = ArrayProp(tmpl, "SelectorTabs", &eTabs);
    if (!labels || !funcs || !tabs || eLabels != 24 || eFuncs != 16 || eTabs != 8) {
        LOG("%s: arrays not found (elem %d/%d/%d), not patched", tn, eLabels, eFuncs, eTabs);
        return -1;
    }
    const int32_t n = labels->num;
    if (n < 2 || funcs->num != n || tabs->num != n) {
        LOG("%s: array counts differ (%d/%d/%d), not patched", tn, n, funcs->num, tabs->num);
        return -1;
    }
    int32_t opt = -1;
    for (int32_t i = 0; i < n; ++i) {
        char s[64];
        Ascii(g_textToString(labels->data + i * 24), s, sizeof s);
        if (strcasecmp(s, "options") == 0) opt = i;
        if (strcasecmp(s, kModsLabel) == 0) {  // already present
            if (slot) slot->modsIndex = i;
            return i;
        }
    }
    if (opt < 0) { LOG("%s: no options entry, not patched", tn); return -1; }
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
    if (slot) slot->modsIndex = ins;
    return ins;
}

// Menu object -> "mods" index, from the SetTemplate that built it. Used to recognise our button's press.
struct MenuIdx { void* menu; int32_t modsIndex; };
static MenuIdx g_menus[4];

static void RememberMenu(void* menu, int32_t idx) {
    for (MenuIdx& m : g_menus) if (!m.menu || m.menu == menu) { m = {menu, idx}; return; }
    g_menus[0] = {menu, idx};
}

static int32_t MenuModsIndex(const void* menu) {
    for (const MenuIdx& m : g_menus) if (m.menu && m.menu == menu) return m.modsIndex;
    return -1;
}


// ---- Mods screen -----------------------------------------------------------------
// The "mods" button opens a copy of the Options tab (UI_Options_Menu_Tab_SA_Mobile_C, a UI_TabbedLayout_C).
// We keep its first list (ControlsOptions_C, a UI_ScrollingSlotList_C), hide the other sub-tabs, clear the list
// and fill it with our own rows: AddSlotItem(row, Options, Settings) exactly like the game's lists do
// (docs/bp_decompiled.md). Each row's Options.PropertyName is a dummy name, which UGameterSettings ignores
// (Get* return defaults, Set* do nothing), so the game's own row logic can never touch real settings.
static void* g_tab;               // our Options-tab instance; null when no menu holds one
static void* g_list;              // its ControlsOptions_C list
static int g_optTabs = -1;        // options tabs constructed since a patched SetTemplate; -1 = idle
static bool g_needBuild;          // (re)fill the list on the next Tick
static bool g_creating;           // we are adding rows: their Construct is ours
static bool g_tabsDone;           // sub-tab headers relabelled/hidden
static int g_view = -1;           // -1 = mod list, else ModList index of the open mod page
static std::vector<void*> g_origRows;  // the game's own rows in g_list (prototypes for our rows)

static std::string g_modsDir, g_cfgDir;
static modmenu::ModList g_mods;
static std::vector<modmenu::IniDoc> g_docs;  // ini of the open mod page (0 or 1 entries)

enum class RowKind { Selection, Message };
enum class Act { None, OpenMod, ToggleMod, IniKey, Back };
struct Row {
    void* obj;
    RowKind kind;
    Act act;
    size_t mod;         // ModList index (OpenMod/ToggleMod)
    std::string key;    // ini key id (IniKey)
    std::string label, value;
    bool dirty;         // texts are applied on the next Tick, after the game's own init has run
};
static std::vector<Row> g_rows;

static Row* FindRow(const void* obj) {
    for (Row& r : g_rows) if (r.obj == obj) return &r;
    return nullptr;
}

static void* PtrProp(void* obj, const char* name) {
    uint8_t* p = obj ? FindProp(obj, name) : nullptr;
    int32_t off = 0;
    void* v = nullptr;
    return p && Rd(p + UE::Prop_Offset, off) && Rd((uint8_t*)obj + off, v) ? v : nullptr;
}

// True if a widget sits (transitively) inside our list: walk widget parents (Slot->Parent) and each
// widget's Outer chain (panel -> WidgetTree -> list widget).
static bool InOurList(void* row) {
    for (void* w = row; w; w = PtrProp(PtrProp(w, "Slot"), "Parent")) {
        void* o = w;
        for (int i = 0; i < 3 && o; ++i) {
            if (!Rd((uint8_t*)o + UE::Obj_Outer, o)) break;
            if (o && o == g_list) return true;
        }
    }
    return false;
}

static FTextRaw ToText(const std::string& s) {
    std::u16string w(s.size() + 1, u'\0');
    for (size_t i = 0; i < s.size(); ++i) w[i] = (unsigned char)s[i] < 128 ? (char16_t)s[i] : u'?';
    FString f = {w.data(), (int32_t)w.size(), (int32_t)w.size()};
    return g_textFromString(f);
}

static void* FindFn(void* obj, const char* name) {
    FNameRaw nm = {0, 0};
    g_FNameCtor(&nm, name, 0);
    void* cls = obj ? ClassOf(obj) : nullptr;
    uint64_t key;
    memcpy(&key, &nm, sizeof key);
    return nm.Index > 0 && cls ? g_findFn(cls, key, 1) : nullptr;
}

// Calls UFunction name on obj with a zeroed parameter block starting with args.
static void CallFn(void* obj, const char* name, const void* args, size_t n) {
    void* fn = FindFn(obj, name);
    if (!fn) { LOG("no function %s", name); return; }
    alignas(16) uint8_t parms[256] = {};
    memcpy(parms, args, n);
    o_ProcessEvent(obj, fn, parms);
}

static void CallText(void* obj, const char* name, const std::string& text) {
    FTextRaw t = ToText(text);
    CallFn(obj, name, &t, sizeof t);
}

static void SetTextProp(void* obj, const char* prop, const std::string& text) {
    if (void* tb = PtrProp(obj, prop)) CallText(tb, "SetText", text);
}

static void SetVisible(void* w, bool on) {
    uint8_t v = on ? 0 : 1;  // ESlateVisibility: Visible / Collapsed
    CallFn(w, "SetVisibility", &v, 1);
}

static int32_t ParmOffset(void* fn, const char* name) {
    FNameRaw nm = {0, 0};
    g_FNameCtor(&nm, name, 0);
    uint64_t key;
    memcpy(&key, &nm, sizeof key);
    uint8_t* p = nm.Index > 0 ? (uint8_t*)g_findProp(fn, key) : nullptr;
    int32_t off = -1;
    return p && Rd(p + UE::Prop_Offset, off) ? off : -1;
}

// ---- Values ------------------------------------------------------------------------
static std::string KeyValueText(const modmenu::IniKey& k) {
    if (k.kind == modmenu::ValueKind::Bool) return k.value == "1" ? "ON" : "OFF";
    if (k.kind == modmenu::ValueKind::Choice)
        for (const modmenu::Label& l : k.labels) if (std::to_string(l.value) == k.value) return l.text;
    return k.value;
}

static std::string ModState(size_t i) { return g_mods.At(i).enabled ? "ON" : "OFF"; }

// Steps an ini key (Bool toggles, Choice cycles its labels) and saves the file.
static void StepKey(const std::string& id, int dir) {
    if (g_docs.empty()) return;
    modmenu::IniDoc& doc = g_docs[0];
    const modmenu::IniKey* k = doc.Find(id);
    if (!k) return;
    std::string next;
    if (k->kind == modmenu::ValueKind::Bool) {
        next = k->value == "1" ? "0" : "1";
    } else if (k->kind == modmenu::ValueKind::Choice && !k->labels.empty()) {
        const std::vector<modmenu::Label>& L = k->labels;
        const int n = (int)L.size();
        int i = -1;
        for (int j = 0; j < n; ++j) if (std::to_string(L[j].value) == k->value) i = j;
        if (i < 0) i = dir > 0 ? -1 : n;
        next = std::to_string(L[((i + dir) % n + n) % n].value);
    } else {
        return;  // shortcut: numbers/strings are read-only here; add slider rows when a range source exists
    }
    if (!doc.Set(id, next) || !doc.Save()) LOG("could not save %s", id.c_str());
}

// ---- Building rows -------------------------------------------------------------------
struct Proto { void* cls; uint8_t* options; };  // a live row of that kind; options points into it

static bool TakeProto(void* row, Proto& p) {
    uint8_t* prop = FindProp(row, "Options");
    int32_t off = 0;
    if (!prop || !Rd(prop + UE::Prop_Offset, off)) return false;
    p = {ClassOf(row), (uint8_t*)row + off};
    return true;
}

static void* (*g_wblCreate)(void* worldCtx, void* cls, void* owningPlayer);  // UWidgetBlueprintLibrary::Create
static FNameRaw g_dummyName;

// Creates a row of proto's class and adds it with a copy of proto's Options whose PropertyName is the dummy.
static void AddRow(const Proto& proto, RowKind kind, Act act, size_t mod, const std::string& key,
                   const std::string& label, const std::string& value) {
    void* addFn = FindFn(g_list, "AddSlotItem");
    if (!addFn || !g_wblCreate) return;
    const int32_t oItem = ParmOffset(addFn, "Slot Item"), oOpts = ParmOffset(addFn, "Options");
    int32_t size = 0;
    Rd((uint8_t*)addFn + 0x58, size);  // UStruct::PropertiesSize
    uint8_t* structObj = nullptr;  // FStructProperty::Struct of the Options parameter
    {
        FNameRaw nm = {0, 0};
        g_FNameCtor(&nm, "Options", 0);
        uint64_t k;
        memcpy(&k, &nm, sizeof k);
        uint8_t* p = (uint8_t*)g_findProp(addFn, k);
        if (p) Rd(p + UE::Prop_Inner, structObj);
    }
    int32_t oName = structObj ? ParmOffset(structObj, "PropertyName") : -1;
    if (oItem < 0 || oOpts < 0 || oName < 0 || size <= 0 || size > 1024) {
        LOG("AddSlotItem layout unexpected (%d %d %d %d)", oItem, oOpts, oName, size);
        return;
    }
    g_creating = true;
    void* row = g_wblCreate(g_list, proto.cls, nullptr);
    if (!row) { g_creating = false; LOG("create row failed"); return; }
    alignas(16) uint8_t parms[1024] = {};
    memcpy(parms + oItem, &row, 8);
    // Shallow copy of a live row's Options; the callee deep-copies it into the new row (Slot.Options = Options).
    memcpy(parms + oOpts, proto.options, 0x90);
    memcpy(parms + oOpts + oName, &g_dummyName, sizeof g_dummyName);
    o_ProcessEvent(g_list, addFn, parms);  // SettingsObject stays null
    g_creating = false;
    g_rows.push_back(Row{row, kind, act, mod, key, label, value, true});
}

static void Render(Row& r) {
    if (r.kind == RowKind::Message) {
        CallText(r.obj, "OverrideMainText", r.label);
        CallText(r.obj, "OverrideAltText", r.value);
        return;
    }
    SetTextProp(r.obj, "OptionNameText", r.label);
    SetTextProp(r.obj, "SelectionText", r.value);
}

// Clears the list and fills it with the current view: the mod list, or one mod's page.
static void Build() {
    Proto sel = {}, msg = {};
    auto consider = [&](void* row) {
        char cn[128];
        NameStr(ClassOf(row), cn, sizeof cn);
        if (!sel.cls && (strstr(cn, "_Selection_") || strstr(cn, "_Bool_"))) TakeProto(row, sel);
        if (!msg.cls && strstr(cn, "_Message_")) TakeProto(row, msg);
    };
    for (Row& r : g_rows) consider(r.obj);
    for (void* r : g_origRows) consider(r);
    if (!sel.cls || !msg.cls) { LOG("no row prototypes yet"); return; }

    CallFn(g_list, "ClearSlotList", nullptr, 0);
    std::vector<Row> old;
    old.swap(g_rows);  // the old rows stay alive until GC; protos above point into them for this call
    g_origRows.clear();

    g_mods.Scan(g_modsDir);
    if (g_view >= (int)g_mods.Count()) g_view = -1;
    if (g_view < 0) {
        char head[96];
        snprintf(head, sizeof head, "%zu mod%s installed", g_mods.Count(), g_mods.Count() == 1 ? "" : "s");
        AddRow(msg, RowKind::Message, Act::None, 0, "", head, "changes apply after restart");
        for (size_t i = 0; i < g_mods.Count(); ++i)
            AddRow(sel, RowKind::Selection, Act::OpenMod, i, "", g_mods.At(i).name, ModState(i) + "  >");
    } else {
        const size_t m = (size_t)g_view;
        AddRow(msg, RowKind::Message, Act::Back, 0, "", "<  back", g_mods.At(m).name);
        AddRow(sel, RowKind::Selection, Act::ToggleMod, m, "", "enabled", ModState(m));
        g_docs.clear();
        modmenu::IniDoc cfg;
        std::map<std::string, std::string> links;
        if (cfg.Load(g_cfgDir + "/modmenu.ini")) links = modmenu::ParseLinks(cfg);
        const std::string ini = modmenu::FindIni(g_mods.At(m).name, modmenu::ListConfigs(g_cfgDir), links);
        if (!ini.empty()) {
            g_docs.emplace_back();
            if (!g_docs[0].Load(g_cfgDir + "/" + ini)) { LOG("could not read %s", ini.c_str()); g_docs.clear(); }
        }
        if (g_docs.empty()) AddRow(msg, RowKind::Message, Act::None, 0, "", "no settings file", "");
        else
            for (const modmenu::IniKey& k : g_docs[0].Keys()) {
                const bool editable = k.kind == modmenu::ValueKind::Bool ||
                                      (k.kind == modmenu::ValueKind::Choice && !k.labels.empty());
                if (editable) AddRow(sel, RowKind::Selection, Act::IniKey, m, k.id, k.id, KeyValueText(k));
                else AddRow(msg, RowKind::Message, Act::None, m, k.id, k.id, k.value);
            }
    }
    LOG("view %d: %zu rows", g_view, g_rows.size());
}

// Sub-tab headers: rename the first ("mods (N)"), hide the rest. Children of UI_TabbedLayout_C.TabButtons
// are UI_Button_Base_C labelled via SetButtonTextValue(Text, JustRebuild=false).
static void FixTabs() {
    void* box = PtrProp(g_tab, "TabButtons");
    uint8_t* sp = box ? FindProp(box, "Slots") : nullptr;
    int32_t off = 0;
    TArrayRaw slots = {};
    if (!sp || !Rd(sp + UE::Prop_Offset, off) || !Rd((uint8_t*)box + off, slots) || slots.num < 1) return;
    for (int i = 0; i < slots.num && i < 16; ++i) {
        void* slot = nullptr;
        void* btn = Rd(slots.data + i * 8, slot) ? PtrProp(slot, "Content") : nullptr;
        if (!btn) return;
        if (i == 0) {
            char t[32];
            snprintf(t, sizeof t, "mods (%zu)", g_mods.Count());
            struct { FTextRaw text; bool justRebuild; } args = {ToText(t), false};
            CallFn(btn, "SetButtonTextValue", &args, sizeof args);
        } else {
            SetVisible(btn, false);
        }
    }
    g_tabsDone = true;
}

static void Activate(Row& r, int dir) {  // dir: +1 next / -1 previous / 0 body tap
    switch (r.act) {
    case Act::OpenMod:
        if (dir == 0) { g_view = (int)r.mod; g_needBuild = true; return; }
        /* fallthrough: arrows toggle the mod from the list */
    case Act::ToggleMod:
        if (!g_mods.SetEnabled(r.mod, !g_mods.At(r.mod).enabled)) LOG("could not toggle %s", g_mods.At(r.mod).name.c_str());
        r.value = ModState(r.mod) + (r.act == Act::OpenMod ? "  >" : "");
        LOG("mod %s -> %s", g_mods.At(r.mod).name.c_str(), ModState(r.mod).c_str());
        break;
    case Act::IniKey:
        StepKey(r.key, dir ? dir : 1);
        if (const modmenu::IniKey* k = g_docs.empty() ? nullptr : g_docs[0].Find(r.key)) r.value = KeyValueText(*k);
        LOG("%s -> %s", r.key.c_str(), r.value.c_str());
        break;
    case Act::Back:
        g_view = -1;
        g_needBuild = true;
        return;
    case Act::None:
        return;
    }
    r.dirty = true;
}

static void StopCapture() {
    g_tab = g_list = nullptr;
    g_rows.clear();
    g_origRows.clear();
    g_docs.clear();
    g_needBuild = false;
}

// Write detector: logs any settings write while our screen exists (should never happen).
typedef void (*SetInt_Fn)(void*, FNameRaw, int32_t);
typedef void (*SetBool_Fn)(void*, FNameRaw, bool);
typedef void (*SetFloat_Fn)(void*, FNameRaw, float);
static SetInt_Fn o_SetInt;
static SetBool_Fn o_SetBool;
static SetFloat_Fn o_SetFloat;
static void LogWrite(const char* what, FNameRaw n) {
    if (!g_tab) return;
    char buf[1024];
    g_FNamePlain(&n, &buf);
    LOG("GameterSettings::%s(%s) while mods screen open", what, buf);
}
static void H_SetInt(void* s, FNameRaw n, int32_t v) { LogWrite("SetIntValue", n); o_SetInt(s, n, v); }
static void H_SetBool(void* s, FNameRaw n, bool v) { LogWrite("SetBoolValue", n); o_SetBool(s, n, v); }
static void H_SetFloat(void* s, FNameRaw n, float v) { LogWrite("SetFloatValue", n); o_SetFloat(s, n, v); }

// Which click binding fired on a row: +1 forward arrow, -1 back arrow, 0 body, 2 not a click.
static int ClickDir(void* func) {
    char n[160];
    NameStr(func, n, sizeof n);
    if (strncmp(n, "BndEvt__", 8) != 0 || !strstr(n, "OnButtonClickedEvent")) return 2;
    if (strstr(n, "ForwardButton")) return 1;
    if (strstr(n, "BackButton")) return -1;
    return 0;
}

// Returns true when the game's own handler must not run for this call.
static bool Observe(void* obj, void* func, void* parms) {
    int32_t fn = 0;
    if (!Rd((uint8_t*)func + UE::Obj_Name, fn)) return false;

    if (Is(fn, N_SetTemplate)) {
        void* tmpl = nullptr;
        if (parms && Rd(parms, tmpl) && tmpl) {
            const int32_t idx = PatchTemplate(tmpl);
            if (idx >= 0) { RememberMenu(obj, idx); g_optTabs = 0; }  // tabs get built from the template inside this call
        }
        return false;
    }
    if (Is(fn, N_InteractedButton)) {
        int32_t v[2] = {0, 0};  // type (0 = click), index
        if (parms) Rd(parms, v);
        const int32_t want = MenuModsIndex(obj);
        if (v[0] == 0 && want >= 0 && v[1] == want) { g_view = -1; g_needBuild = g_tab != nullptr; }
        return false;
    }
    // Our tab is the 2nd options-tab instance built after a patched SetTemplate (index order: options < mods).
    if (Is(fn, N_Construct) && g_optTabs >= 0 && ClassIs(obj, "UI_Options_Menu_Tab_SA_Mobile_C") && ++g_optTabs == 2) {
        g_optTabs = -1;
        StopCapture();
        g_tab = obj;
        g_tabsDone = false;
        return false;
    }
    if (!g_tab) return false;
    if (Is(fn, N_Destruct) && obj == g_tab) { StopCapture(); return false; }
    if (Row* r = FindRow(obj)) {
        if (Is(fn, N_Destruct)) { g_rows.erase(g_rows.begin() + (r - g_rows.data())); return false; }
        if (Is(fn, N_FinalCheck) || Is(fn, N_UpdateSettings) || Is(fn, N_NextSel) || Is(fn, N_PrevSel) ||
            Is(fn, N_InteractedSel))
            return true;
        // Body tap: SlotButton.OnClicked is bound to OnSlotClicked (docs/bp_decompiled.md).
        const int dir = Is(fn, N_OnSlotClicked) ? 0 : ClickDir(func);
        if (dir == 2) return false;
        // One tap fires both OnSlotClicked and the button's BndEvt click: act once per tap.
        static void* lastRow;
        static int64_t lastMs;
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (!(obj == lastRow && now - lastMs < 150)) Activate(*r, dir);
        lastRow = obj;
        lastMs = now;
        return true;
    }
    return false;
}

// After the game handled a call: track the tab's first list and the game's rows in it.
static void AfterCall(void* obj, void* func) {
    int32_t fn = 0;
    if (!Rd((uint8_t*)func + UE::Obj_Name, fn)) return;
    if (Is(fn, N_Construct)) {
        char cn[128];
        NameStr(ClassOf(obj), cn, sizeof cn);
        if (!g_list && strcmp(cn, "ControlsOptions_C") == 0) { g_list = obj; return; }
        if (g_creating || !g_list || strncmp(cn, "UI_SlotButton_", 14) != 0 || FindRow(obj)) return;
        if (!InOurList(obj)) return;
        g_origRows.push_back(obj);  // the game (re)filled our list: replace its rows with ours
        g_needBuild = true;
        return;
    }
    if (Is(fn, N_Tick)) {
        if (g_needBuild && g_list) { g_needBuild = false; Build(); }
        if (!g_tabsDone) FixTabs();
        for (Row& r : g_rows) if (r.dirty) { r.dirty = false; Render(r); }
    }
}

static void Hooked_ProcessEvent(void* obj, void* func, void* parms) {
    if (!obj || !func) { o_ProcessEvent(obj, func, parms); return; }
    if (Observe(obj, func, parms)) return;
    o_ProcessEvent(obj, func, parms);
    if (g_tab) AfterCall(obj, func);
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
    void *pe, *ctor, *plain, *find, *findFn, *toStr, *fromStr, *mal, *real, *create;
    struct Sym { const char* sym; void** out; } syms[] = {
        {"_ZN7UObject12ProcessEventEP9UFunctionPv", &pe},
        {"_ZN5FNameC1EPKc9EFindName", &ctor},
        {"_ZNK5FName18GetPlainANSIStringERA1024_c", &plain},
        {"_ZNK7UStruct18FindPropertyByNameE5FName", &find},
        {"_ZNK6UClass18FindFunctionByNameE5FNameN17EIncludeSuperFlag4TypeE", &findFn},
        {"_ZNK5FText8ToStringEv", &toStr},
        {"_ZN5FText10FromStringERK7FString", &fromStr},
        {"_ZN7FMemory6MallocEyj", &mal},
        {"_ZN7FMemory7ReallocEPvyj", &real},
        {"_ZN23UWidgetBlueprintLibrary6CreateEP7UObject11TSubclassOfI11UUserWidgetEP17APlayerController", &create},
    };
    for (Sym& s : syms)
        if (!(*s.out = (void*)aml->GetSym(h, s.sym))) { LOG("ABORT: missing %s", s.sym); return; }
    g_FNameCtor = (FNameCtor_Fn)ctor;
    g_FNamePlain = (FNamePlain_Fn)plain;
    g_findProp = (FindProp_Fn)find;
    g_findFn = (FindFn_Fn)findFn;
    g_textToString = (TextToString_Fn)toStr;
    g_textFromString = (TextFromString_Fn)fromStr;
    g_malloc = (Malloc_Fn)mal;
    g_realloc = (Realloc_Fn)real;
    g_wblCreate = (void* (*)(void*, void*, void*))create;
    g_FNameCtor(&g_dummyName, "GTAML_Dummy", 1);  // FNAME_Add
    g_modsDir = std::string(aml->GetAndroidDataRootPath()) + "/mods";  // where AML loads *.so from
    g_cfgDir = aml->GetConfigPath();
    if (!aml->Hook(pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent)) { LOG("ABORT: hook failed"); return; }
    const struct { const char* sym; void* fn; void** orig; } writes[] = {
        {"_ZN16UGameterSettings11SetIntValueE5FNamei", (void*)&H_SetInt, (void**)&o_SetInt},
        {"_ZN16UGameterSettings12SetBoolValueE5FNameb", (void*)&H_SetBool, (void**)&o_SetBool},
        {"_ZN16UGameterSettings13SetFloatValueE5FNamef", (void*)&H_SetFloat, (void**)&o_SetFloat},
    };
    for (const auto& w : writes) {
        void* a = (void*)aml->GetSym(h, w.sym);
        if (!a || !aml->Hook(a, w.fn, w.orig)) LOG("write detector: cannot hook %s", w.sym);
    }
    pthread_t th;
    pthread_create(&th, nullptr, ResolveThread, nullptr);
    pthread_detach(th);
    LOG("installed");
}
