// ModMenu: adds a "mods" entry to the pause/main menu. Pressing it opens the Options tab's rows
// as a mods screen: each row shows a mod's on/off state or an ini key's value.
// Logcat tag: ModMenu.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
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

enum NameId { N_SetTemplate, N_InteractedButton, N_Construct, N_Destruct, N_FinalCheck, N_OptionChanged,
              N_OnOptionSelChanged, N_OnOptionPressed, N_SlotButtonPressed, N_Tick, N_COUNT };
static const char* const kNames[N_COUNT] = {"SetTemplate", "InteractedButton", "Construct", "Destruct",
                                            "On Option Changed Final Check", "OptionChanged",
                                            "OnOptionSelectionChanged", "OnOptionPressed", "SlotButtonPressed", "Tick"};
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

// ---- Mods screen ---------------------------------------------------------------
// Row widgets of the Options tab's five lists (page order). Each list is built with its own rows.
static const char* const kLists[5] = {"ControlsOptions_C", "GameplayOptions_C", "GraphicsOptions_C",
                                      "AudioOptions_C", "LanguageOptions_C"};
static void* g_lists[5];
static void* g_tab;               // the Options tab instance hosting the mods screen; null when closed
static int g_optTabs = -1;        // options tabs constructed since a patched SetTemplate; -1 = idle
static bool g_noteShown;          // the page-0 header row carries the restart note

static std::string g_modsDir, g_cfgDir;
static modmenu::ModList g_mods;
static std::vector<modmenu::IniDoc> g_docs;  // one per linked ini file; Entry::doc indexes it

enum class RowKind { Bool, Selection, Message, Slider, Blank };

struct Entry {
    bool isMod = false;
    size_t mod = 0;        // ModList index when isMod
    size_t doc = 0;        // g_docs index otherwise
    std::string id;        // ini key id
    std::string label;     // row caption
    modmenu::ValueKind kind = modmenu::ValueKind::Bool;
    bool assigned = false;
};
static std::vector<Entry> g_entries;

struct Row {
    void* obj;
    RowKind kind;
    int page;
    int entry;  // index into g_entries, -1 when the row shows nothing
    bool note = false;   // page-0 header carrying the restart note
    bool dirty = false;  // texts must be (re)applied on the next Tick: the game sets its own after Construct
};
static std::vector<Row> g_rows;

static bool KindOf(const char* cn, RowKind& k) {
    static const struct { const char* part; RowKind kind; } kinds[] = {
        {"_Bool_", RowKind::Bool}, {"_Selection_", RowKind::Selection}, {"_Message_", RowKind::Message},
        {"_Slider_", RowKind::Slider}, {"_Blank_", RowKind::Blank}};
    for (const auto& e : kinds) if (strstr(cn, e.part)) { k = e.kind; return true; }
    return false;
}

static int PageOf(const char* cn) {
    for (int i = 0; i < 5; ++i) if (strcmp(cn, kLists[i]) == 0) return i;
    return -1;
}

static bool IsList(const void* obj) {
    for (void* l : g_lists) if (l && l == obj) return true;
    return false;
}

static Row* FindRow(const void* obj) {
    for (Row& r : g_rows) if (r.obj == obj) return &r;
    return nullptr;
}

// Row pointer stored in an event's parameter block at byte offset off.
static Row* RowAt(void* parms, size_t off) {
    void* p = nullptr;
    return parms && Rd((uint8_t*)parms + off, p) ? FindRow(p) : nullptr;
}

static void* PtrProp(void* obj, const char* name) {
    uint8_t* p = obj ? FindProp(obj, name) : nullptr;
    int32_t off = 0;
    void* v = nullptr;
    return p && Rd(p + UE::Prop_Offset, off) && Rd((uint8_t*)obj + off, v) ? v : nullptr;
}

// Page of the list that owns a row: walk widget parents (Slot->Parent), testing each widget's Outer chain
// (panel -> WidgetTree -> list widget). -1 for rows of any other list, e.g. the real Options tab.
static int PageOfRow(void* row) {
    for (void* w = row; w; w = PtrProp(PtrProp(w, "Slot"), "Parent")) {
        void* o = w;
        for (int i = 0; i < 3 && o; ++i) {
            if (!Rd((uint8_t*)o + UE::Obj_Outer, o)) break;
            for (int p = 0; p < 5; ++p) if (o && o == g_lists[p]) return p;
        }
    }
    return -1;
}

static bool Fits(const Entry& e, RowKind k) {
    switch (e.kind) {
    case modmenu::ValueKind::Bool: return k == RowKind::Bool || k == RowKind::Selection;
    case modmenu::ValueKind::Choice: return k == RowKind::Selection;
    default: return k == RowKind::Message;  // Int, Float, String
    }
}

// ---- Calls into the game (bypass our hook) ----------------------------------------
static FTextRaw ToText(const std::string& s) {
    std::u16string w(s.size() + 1, u'\0');  // NUL-terminated
    for (size_t i = 0; i < s.size(); ++i) w[i] = (unsigned char)s[i] < 128 ? (char16_t)s[i] : u'?';
    FString f = {w.data(), (int32_t)w.size(), (int32_t)w.size()};
    return g_textFromString(f);
}

// Calls UFunction name on obj with a zeroed 128-byte parameter block holding args.
static void CallFn(void* obj, const char* name, const void* args, size_t n) {
    FNameRaw nm = {0, 0};
    g_FNameCtor(&nm, name, 0);
    void* cls = ClassOf(obj);
    uint64_t key;
    memcpy(&key, &nm, sizeof key);
    void* fn = nm.Index > 0 && cls ? g_findFn(cls, key, 1) : nullptr;
    if (!fn) { LOG("no function %s", name); return; }
    alignas(16) uint8_t parms[128] = {};
    memcpy(parms, args, n);
    o_ProcessEvent(obj, fn, parms);
}

static void CallText(void* obj, const char* name, const std::string& text) {
    FTextRaw t = ToText(text);
    CallFn(obj, name, &t, sizeof t);
}

static void SetTextProp(void* obj, const char* prop, const std::string& text) {
    uint8_t* p = FindProp(obj, prop);
    int32_t off = 0;
    void* tb = nullptr;
    if (!p || !Rd(p + UE::Prop_Offset, off) || !Rd((uint8_t*)obj + off, tb) || !tb) return;
    CallText(tb, "SetText", text);
}

static void Collapse(void* obj) {
    uint8_t collapsed = 1;  // ESlateVisibility::Collapsed
    CallFn(obj, "SetVisibility", &collapsed, 1);
}

// ---- Entries and values -----------------------------------------------------------
static std::string ValueText(const Entry& e) {
    if (e.isMod) return g_mods.At(e.mod).enabled ? "ON" : "OFF";
    const modmenu::IniKey* k = g_docs[e.doc].Find(e.id);
    if (!k) return "?";
    if (e.kind == modmenu::ValueKind::Bool) return k->value == "1" ? "ON" : "OFF";
    if (e.kind == modmenu::ValueKind::Choice)
        for (const modmenu::Label& l : k->labels) if (std::to_string(l.value) == k->value) return l.text;
    return k->value;
}

static void Render(const Row& r) {
    if (r.note) { CallText(r.obj, "OverrideMainText", "Mod on/off takes effect after restart"); return; }
    if (r.entry < 0) return;
    const Entry& e = g_entries[r.entry];
    if (r.kind == RowKind::Message) { CallText(r.obj, "OverrideMainText", e.id + " = " + ValueText(e)); return; }
    SetTextProp(r.obj, "OptionNameText", e.label);
    SetTextProp(r.obj, "SelectionText", ValueText(e));
}

// Advances an entry one step: toggles a mod or an ini key, then saves the ini file.
static void Step(Entry& e, int dir) {
    if (e.isMod) {
        if (!g_mods.SetEnabled(e.mod, !g_mods.At(e.mod).enabled)) LOG("could not toggle %s", e.label.c_str());
        return;
    }
    modmenu::IniDoc& doc = g_docs[e.doc];
    const modmenu::IniKey* k = doc.Find(e.id);
    if (!k) return;
    std::string next;
    if (e.kind == modmenu::ValueKind::Bool) {
        next = k->value == "1" ? "0" : "1";
    } else {
        const std::vector<modmenu::Label>& L = k->labels;
        const int n = (int)L.size();
        if (n == 0) return;
        int i = -1;
        for (int j = 0; j < n; ++j) if (std::to_string(L[j].value) == k->value) i = j;
        if (i < 0) i = dir > 0 ? -1 : n;
        next = std::to_string(L[((i + dir) % n + n) % n].value);
    }
    if (!doc.Set(e.id, next) || !doc.Save()) LOG("could not save %s", e.id.c_str());
}

// Fills g_entries: every mod first, then the keys of its linked ini file.
static void BuildEntries() {
    g_entries.clear();
    g_docs.clear();
    g_mods.Scan(g_modsDir);
    for (size_t i = 0; i < g_mods.Count(); ++i) {
        Entry e;
        e.isMod = true;
        e.mod = i;
        e.label = g_mods.At(i).name;
        e.kind = modmenu::ValueKind::Bool;
        g_entries.push_back(e);
    }

    modmenu::IniDoc cfg;
    std::map<std::string, std::string> links;
    if (cfg.Load(g_cfgDir + "/modmenu.ini")) links = modmenu::ParseLinks(cfg);
    const std::vector<std::string> inis = modmenu::ListConfigs(g_cfgDir);
    std::vector<std::string> used;
    for (size_t i = 0; i < g_mods.Count(); ++i) {
        const std::string ini = modmenu::FindIni(g_mods.At(i).name, inis, links);
        if (ini.empty() || std::find(used.begin(), used.end(), ini) != used.end()) continue;
        used.push_back(ini);
        g_docs.emplace_back();
        if (!g_docs.back().Load(g_cfgDir + "/" + ini)) {
            LOG("could not read %s", ini.c_str());
            g_docs.pop_back();
            continue;
        }
        for (const modmenu::IniKey& k : g_docs.back().Keys()) {
            Entry e;
            e.doc = g_docs.size() - 1;
            e.id = k.id;
            e.label = k.id;
            e.kind = k.kind;
            g_entries.push_back(e);
        }
    }
}

static void StopCapture() {
    size_t left = 0;
    for (const Entry& e : g_entries) left += !e.assigned;
    if (g_tab && left) LOG("mods screen: %zu entries did not fit", left);
    g_tab = nullptr;
    for (void*& l : g_lists) l = nullptr;
    g_rows.clear();
    g_entries.clear();
    g_docs.clear();
}

static void StartCapture(void* tab) {
    StopCapture();
    g_tab = tab;
    g_noteShown = false;
    BuildEntries();
    LOG("mods screen: %zu mods, %zu entries", g_mods.Count(), g_entries.size());
}

// Assigns the first free entry that fits a row, or collapses the row. Called after the row's Construct.
static void Populate(size_t ri) {
    Row& row = g_rows[ri];
    if (row.kind == RowKind::Slider || row.kind == RowKind::Blank) { Collapse(row.obj); return; }
    if (row.kind == RowKind::Message && row.page == 0 && !g_noteShown) {
        g_noteShown = true;
        row.note = row.dirty = true;
        return;
    }
    for (size_t i = 0; i < g_entries.size(); ++i) {
        Entry& e = g_entries[i];
        if (e.assigned || !Fits(e, row.kind)) continue;
        e.assigned = true;
        row.entry = (int)i;
        break;
    }
    if (row.entry < 0) { Collapse(row.obj); return; }
    row.dirty = true;
}

// Which arrow binding fired: +1 forward, -1 back, 0 none.
static int ArrowDir(void* func) {
    char n[160];
    NameStr(func, n, sizeof n);
    if (strncmp(n, "BndEvt__", 8) != 0 || !strstr(n, "OnButtonClickedEvent")) return 0;
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
        if (v[0] == 0 && want >= 0 && v[1] == want) LOG("mods button pressed");
        return false;
    }
    // Our tab is the 2nd options-tab instance built after a patched SetTemplate (index order: options < mods).
    if (Is(fn, N_Construct) && g_optTabs >= 0 && ClassIs(obj, "UI_Options_Menu_Tab_SA_Mobile_C") && ++g_optTabs == 2) {
        g_optTabs = -1;
        StartCapture(obj);
        return false;
    }
    if (!g_tab) return false;
    if (Is(fn, N_Destruct) && obj == g_tab) { StopCapture(); return false; }
    if (IsList(obj)) {  // our lists: the commit path would write game settings by name
        if (Is(fn, N_OptionChanged) || Is(fn, N_OnOptionSelChanged)) return true;
        if (Is(fn, N_OnOptionPressed)) return RowAt(parms, 0) != nullptr;
        if (Is(fn, N_SlotButtonPressed)) return RowAt(parms, 8) != nullptr;
        return false;
    }
    if (Row* r = FindRow(obj)) {
        if (Is(fn, N_Destruct)) {  // the list rebuilds its rows: free the entry for the replacement row
            if (r->entry >= 0) g_entries[r->entry].assigned = false;
            g_rows.erase(g_rows.begin() + (r - g_rows.data()));
            return false;
        }
        if (Is(fn, N_FinalCheck)) return true;
        const int dir = ArrowDir(func);
        if (dir && r->entry >= 0 && (r->kind == RowKind::Bool || r->kind == RowKind::Selection)) {
            Step(g_entries[r->entry], dir);
            r->dirty = true;
        }
        return dir != 0;
    }
    return false;
}

// Runs after the game handled a call: captures the lists and rows as the tab builds them.
static void AfterCall(void* obj, void* func) {
    int32_t fn = 0;
    if (!Rd((uint8_t*)func + UE::Obj_Name, fn) || !Is(fn, N_Construct)) return;
    char cn[128];
    NameStr(ClassOf(obj), cn, sizeof cn);
    // Our tab's lists are the first of each class constructed after the tab itself.
    if (const int p = PageOf(cn); p >= 0) { if (!g_lists[p]) g_lists[p] = obj; return; }
    RowKind k;
    if (!KindOf(cn, k)) return;
    const int page = PageOfRow(obj);
    if (page < 0 || FindRow(obj)) return;
    g_rows.push_back(Row{obj, k, page, -1});
    Populate(g_rows.size() - 1);
    LOG("row p%d %s -> entry %d", page, cn, g_rows.back().entry);
}

static void Hooked_ProcessEvent(void* obj, void* func, void* parms) {
    if (!obj || !func) { o_ProcessEvent(obj, func, parms); return; }
    if (Observe(obj, func, parms)) return;
    o_ProcessEvent(obj, func, parms);
    if (!g_tab) return;
    AfterCall(obj, func);
    if (Row* r = FindRow(obj)) r->dirty = true;  // any row event may have re-set the game's texts
    int32_t fn = 0;
    if (Rd((uint8_t*)func + UE::Obj_Name, fn) && Is(fn, N_Tick))
        for (Row& r : g_rows) if (r.dirty) { r.dirty = false; Render(r); }
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
    void *pe, *ctor, *plain, *find, *findFn, *toStr, *fromStr, *mal, *real;
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
    g_modsDir = std::string(aml->GetAndroidDataRootPath()) + "/mods";  // where AML loads *.so from
    g_cfgDir = aml->GetConfigPath();
    if (!aml->Hook(pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent)) { LOG("ABORT: hook failed"); return; }
    pthread_t th;
    pthread_create(&th, nullptr, ResolveThread, nullptr);
    pthread_detach(th);
    LOG("installed");
}
