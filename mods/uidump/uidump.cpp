// UIDump: read-only UMG diagnostics for GTA SA DE (UE 4.26, arm64, AML).
// Logs menu-like class/function pairs, the widget tree of the pause and main menus, the menu template
// passed to SetTemplate, custom-function parameters, and class reflection (CLS lines) to logcat tag UIDUMP.
// Never changes behaviour: every ProcessEvent call is forwarded to the original.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <mod/amlmod.h>
#include "../../sdk/gtade.h"

MYMOD(net.gtamod.uidump, UIDump, 0.1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

// Validated read of one T at p; false if unmapped.
template <class T> static inline bool Rd(const void* p, T& out) {
    return gtade::SafeRead(p, &out, sizeof(T));
}

constexpr int kMaxLines = 6000;     // total UIDUMP log lines per process
constexpr int kMaxMembers = 512;    // props/functions/params walked per class
constexpr int kMaxClassMarks = 256; // classes dumped per process
constexpr int kMaxDepth = 12;       // widget tree recursion depth
constexpr int kMaxSlots = 256;      // slots read per panel
constexpr int kOuterHops = 8;       // Outer chain length in a pair log line
constexpr int kSuperHops = 32;      // SuperStruct walk limit
constexpr int kMaxListItems = 64;   // TArray elements logged per template array
constexpr int kMaxStrChars = 512;   // UTF-16 code units read per FString
constexpr int kFNameFind = 0;       // EFindName::FNAME_Find

// UE 4.26 offsets, from docs/reflection_layout.md.
namespace UE {
constexpr size_t Obj_Index = 0x0C, Obj_Class = 0x10, Obj_Name = 0x18, Obj_Outer = 0x20;
constexpr size_t Item_Size = 0x18;
constexpr size_t Struct_Super = 0x40, Struct_Children = 0x48, Struct_ChildProps = 0x50, Struct_PropLink = 0x70;
constexpr size_t UField_Next = 0x28, FField_Next = 0x20, FField_Name = 0x28;
constexpr size_t Prop_Sub = 0x78, Prop_BoolByte = 0x79, Prop_BoolMask = 0x7B;
constexpr size_t Field_Class = 0x08;                                    // FField::ClassPrivate
constexpr size_t Prop_ElemSize = 0x38, Prop_Flags = 0x40, Prop_Inner = 0x78;
constexpr size_t Prop_Offset = 0x4C, Prop_NextLink = 0x58;
constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;
}

typedef void (*ProcessEvent_Fn)(void* obj, void* func, void* parms);
typedef void (*FNameCtor_Fn)(void* fname, const char* name, int findType);
typedef void (*FNamePlain_Fn)(void* fname, char (*buf)[1024]);
typedef void* (*FindProp_Fn)(const void* structObj, uint64_t fname);  // UStruct::FindPropertyByName(FName) const
typedef const void* (*TextToString_Fn)(const void* text);            // FText::ToString() const -> const FString&

static ProcessEvent_Fn o_ProcessEvent;
static FNameCtor_Fn g_FNameCtor;
static FNamePlain_Fn g_FNamePlain;
static FindProp_Fn g_findProp;
static TextToString_Fn g_textToString;
static uint8_t* g_objects;

// Names the hooks match on. expectedOff: documented property offset to check against reflection, -1 if none.
enum NameId : int {
    N_Construct, N_WidgetTree, N_RootWidget, N_Slots, N_Content, N_Text,
    N_SetTemplate, N_CustomStr, N_CustomStrText, N_CustomBoolRet, N_InteractedButton,
    N_Gameterface, N_LeftButtonTemplate, N_LeftButtons, N_AltLeftButtonText,
    N_LeftButtonVerticalSize, N_SelectorTabs, N_FunctionToCallOnPress,
    N_SliderSlotClass, N_SelectionSlotClass, N_BoolSlotClass, N_MessageSlotClass,
    N_ScrollingSlotList, N_COUNT
};
struct NameDef { const char* str; int expectedOff; };
static const NameDef kNames[N_COUNT] = {
    {"Construct", -1}, {"WidgetTree", -1}, {"RootWidget", -1}, {"Slots", -1}, {"Content", -1}, {"Text", -1},
    {"SetTemplate", -1}, {"CustomFunctionFromString", -1}, {"CustomFunctionFromStringWithText", -1},
    {"CustomFunctionWithBoolReturn", -1}, {"InteractedButton", -1},
    {"Gameterface", 0x290}, {"LeftButtonTemplate", 0x1C8}, {"LeftButtons", 0x1D0},
    {"AltLeftButtonText", 0x1E0}, {"LeftButtonVerticalSize", 0x200}, {"SelectorTabs", 0x248},
    {"FunctionToCallOnPress", 0x258},
    {"SliderSlotClass", 0x980}, {"SelectionSlotClass", 0x988}, {"BoolSlotClass", 0x990},
    {"MessageSlotClass", 0x998}, {"UI_ScrollingSlotList_C", -1},
};
struct FNameRaw { int32_t Index, Number; };  // Index 0 = unresolved
static FNameRaw g_name[N_COUNT];
static uint32_t g_warnMissing, g_warnMismatch;  // one warning per NameId

static const char* const kKeys[] = {"Menu", "Pause", "Frontend", "Button", "WBP_"};

static int g_lines;
static void Out(const char* fmt, ...) {
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "UIDUMP", "%s", buf);
}

static void NameToStr(int32_t idx, char* out, size_t cap) {
    if (idx <= 0) { snprintf(out, cap, "None"); return; }
    FNameRaw n = {idx, 0};
    char buf[1024];
    g_FNamePlain(&n, &buf);
    snprintf(out, cap, "%s", buf);
}

static uint8_t* ObjectItem(int32_t index) {
    if (!g_objects || index < 0) return nullptr;
    uint8_t** chunks = nullptr;
    int32_t num = 0;
    if (!Rd(g_objects, chunks) || !chunks || !Rd(g_objects + 0x14, num) || index >= num) return nullptr;
    uint8_t* chunk = nullptr;
    if (!Rd(chunks + index / 65536, chunk) || !chunk) return nullptr;
    return chunk + (index % 65536) * UE::Item_Size;
}

static bool IsLive(const void* obj) {
    if (!obj) return false;
    int32_t idx = 0;
    if (!Rd((const uint8_t*)obj + UE::Obj_Index, idx)) return false;
    uint8_t* item = ObjectItem(idx);
    void* ptr = nullptr;
    return item && Rd(item, ptr) && ptr == obj;
}

static void* ClassOf(void* obj) {
    void* cls = nullptr;
    return (obj && Rd((uint8_t*)obj + UE::Obj_Class, cls)) ? cls : nullptr;
}

static void ObjName(void* obj, char* out, size_t cap) {
    int32_t idx = 0;
    if (!obj || !Rd((uint8_t*)obj + UE::Obj_Name, idx)) { snprintf(out, cap, "?"); return; }
    NameToStr(idx, out, cap);
}

static void ClassName(void* obj, char* out, size_t cap) {
    ObjName(ClassOf(obj), out, cap);
}

static bool IsName(int32_t idx, int id) {
    return idx > 0 && __atomic_load_n(&g_name[id].Index, __ATOMIC_ACQUIRE) == idx;
}

static uint64_t NameKey(int id) {
    uint64_t k;
    memcpy(&k, &g_name[id], sizeof k);
    return k;
}

// True if obj's class or one of its SuperStructs is named name (exact, or prefix when prefix is set).
static bool ChainHas(void* cls, const char* name, bool prefix) {
    void* s = cls;
    for (int depth = 0; s && depth < kSuperHops && IsLive(s); ++depth) {
        char n[256];
        ObjName(s, n, sizeof n);
        if (prefix ? strncmp(n, name, strlen(name)) == 0 : strcmp(n, name) == 0) return true;
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
    return false;
}

static bool IsA(void* obj, const char* name) { return ChainHas(ClassOf(obj), name, false); }

static bool IsMenuBase(void* cls) {
    return ChainHas(cls, "UI_Menu_Base", true) || ChainHas(cls, "UUI_Menu_Base", true);
}

// Class-name keyword match, cached per (class, name). Same keywords as before.
struct ClassEntry { void* cls; int32_t name; bool match; };
static ClassEntry g_classCache[512];
static int g_classNext;

static bool MenuLike(void* cls) {
    int32_t clsName = 0;
    if (!Rd((uint8_t*)cls + UE::Obj_Name, clsName)) return false;
    for (const ClassEntry& e : g_classCache)
        if (e.cls == cls && e.name == clsName) return e.match;
    bool match = false;
    void* s = cls;
    for (int depth = 0; s && depth < kSuperHops; ++depth) {
        int32_t n = 0;
        if (!Rd((uint8_t*)s + UE::Obj_Name, n)) break;
        char name[256];
        NameToStr(n, name, sizeof name);
        for (const char* k : kKeys)
            if (strstr(name, k)) match = true;
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
    if (g_classNext < (int)(sizeof g_classCache / sizeof g_classCache[0]))
        g_classCache[g_classNext++] = {cls, clsName, match};
    return match;
}

// Reflected FProperty for kNames[id] on obj's class chain (UStruct::FindPropertyByName up SuperStruct).
static uint8_t* FindProp(void* obj, uint64_t key) {
    void* s = ClassOf(obj);
    for (int depth = 0; s && depth < kSuperHops && IsLive(s); ++depth) {
        if (void* p = g_findProp(s, key)) return (uint8_t*)p;
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
    return nullptr;
}

static uint8_t* ReflProp(void* obj, int id, bool warn) {
    if (__atomic_load_n(&g_name[id].Index, __ATOMIC_ACQUIRE) <= 0 || !obj) return nullptr;
    uint8_t* p = FindProp(obj, NameKey(id));
    if (!p && warn && !(g_warnMissing & (1u << id))) {
        g_warnMissing |= 1u << id;
        char cn[256];
        ClassName(obj, cn, sizeof cn);
        Out("property %s not found on %s", kNames[id].str, cn);
    }
    return p;
}

// Address of property kNames[id] inside obj. The reflected offset is authoritative; a documented-offset mismatch logs once.
static uint8_t* ReflAddr(void* obj, int id, bool warn) {
    uint8_t* p = ReflProp(obj, id, warn);
    int32_t off = 0;
    if (!p || !Rd(p + UE::Prop_Offset, off) || off < 0 || off > 0x10000) return nullptr;
    int exp = kNames[id].expectedOff;
    if (exp >= 0 && off != exp && !(g_warnMismatch & (1u << id))) {
        g_warnMismatch |= 1u << id;
        Out("MISMATCH %s: reflected offset 0x%x, documented 0x%x (using reflected)", kNames[id].str, off, exp);
    }
    return (uint8_t*)obj + off;
}

static void* PropObject(void* obj, int id, bool warn = true) {
    uint8_t* a = ReflAddr(obj, id, warn);
    void* v = nullptr;
    return (a && Rd(a, v)) ? v : nullptr;
}

// Type name of an FProperty (its FFieldClass name, e.g. "ArrayProperty"); "?" if unreadable.
static void FieldTypeName(uint8_t* prop, char* out, size_t cap) {
    void* fc = nullptr;
    int32_t idx = 0;
    if (!Rd(prop + UE::Field_Class, fc) || !fc || !Rd(fc, idx)) { snprintf(out, cap, "?"); return; }
    NameToStr(idx, out, cap);
}

// Element size of a TArray property, from its Inner FProperty; -1 if unreadable.
static int32_t ArrayElemSize(uint8_t* arrProp) {
    uint8_t* inner = nullptr;
    int32_t es = -1;
    if (!Rd(arrProp + UE::Prop_Inner, inner) || !inner || !Rd(inner + UE::Prop_ElemSize, es)) return -1;
    return es;
}

// UTF-16 -> UTF-8. Surrogate pairs are combined; lone surrogates become '?'.
static void Utf16ToUtf8(const char16_t* s, size_t n, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = '?';
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

// Reads a TArray<TCHAR> FString ({Data, Num, Max}) at fstr into UTF-8 out. False if unreadable.
static bool ReadFString(const void* fstr, char* out, size_t cap) {
    out[0] = 0;
    void* data = nullptr;
    int32_t num = 0;
    if (!Rd(fstr, data) || !Rd((const uint8_t*)fstr + 8, num)) return false;
    if (!data || num <= 0) return true;
    if (num > kMaxStrChars) num = kMaxStrChars;
    char16_t buf[kMaxStrChars];
    if (!gtade::SafeRead(data, buf, sizeof(char16_t) * num)) return false;
    size_t n = (size_t)num;
    while (n && buf[n - 1] == 0) --n;  // drop the NUL terminator
    Utf16ToUtf8(buf, n, out, cap);
    return true;
}

// FText -> UTF-8 via FText::ToString. text points at an FText value.
static bool TextToUtf8(const void* text, char* out, size_t cap) {
    uint8_t probe;
    if (!Rd(text, probe)) return false;  // mapped check before a native call
    const void* str = g_textToString(text);
    return str && ReadFString(str, out, cap);
}

enum class ArrKind { Count, Text, Str, Class };

// Logs a TArray property of obj: type, count, element size, and elements per kind.
static void DumpArray(void* obj, int id, ArrKind kind) {
    const char* label = kNames[id].str;
    uint8_t* prop = ReflProp(obj, id, true);
    uint8_t* hdr = ReflAddr(obj, id, true);
    if (!prop || !hdr) return;
    char ft[64];
    FieldTypeName(prop, ft, sizeof ft);
    int32_t es = ArrayElemSize(prop);
    void* data = nullptr;
    int32_t num = 0;
    if (!Rd(hdr, data) || !Rd(hdr + 8, num) || num < 0) { Out("%s: unreadable TArray header", label); return; }
    Out("%s: type=%s count=%d elemSize=%d", label, ft, num, es);
    if (strcmp(ft, "ArrayProperty") != 0) Out("%s: MISMATCH expected ArrayProperty", label);
    if (kind == ArrKind::Count || num == 0 || !data) return;
    const int32_t minSize = kind == ArrKind::Str ? 16 : kind == ArrKind::Class ? 8 : 1;
    if (es < minSize) { Out("%s: MISMATCH elemSize %d < %d", label, es, minSize); return; }
    if (num > kMaxListItems) num = kMaxListItems;
    for (int i = 0; i < num; ++i) {
        const uint8_t* e = (const uint8_t*)data + (size_t)i * es;
        char s[1024] = "";
        bool ok = false;
        switch (kind) {
        case ArrKind::Text:
            ok = TextToUtf8(e, s, sizeof s);
            break;
        case ArrKind::Str:
            ok = ReadFString(e, s, sizeof s);
            break;
        case ArrKind::Class: {
            void* c = nullptr;
            if (Rd(e, c)) {
                ok = true;
                if (!c) snprintf(s, sizeof s, "(null)");
                else if (IsLive(c)) ObjName(c, s, sizeof s);
                else snprintf(s, sizeof s, "<not live @%p>", c);
            }
            break;
        }
        case ArrKind::Count:
            break;
        }
        Out("  [%d] %s", i, ok ? s : "<unreadable>");
    }
}

// Logs the UUI_Menu_Base_BP-derived menu template: class refs, sizes, and button arrays.
static void DumpTemplate(void* tmpl) {
    char tn[256], tc[256];
    ObjName(tmpl, tn, sizeof tn);
    ClassName(tmpl, tc, sizeof tc);
    Out("== template %s class=%s @%p", tn, tc, tmpl);

    if (uint8_t* a = ReflAddr(tmpl, N_LeftButtonTemplate, true)) {
        void* c = nullptr;
        char cn[256] = "(null)";
        if (Rd(a, c) && c) {
            if (IsLive(c)) ObjName(c, cn, sizeof cn);
            else snprintf(cn, sizeof cn, "<not live @%p>", c);
        }
        Out("LeftButtonTemplate=%s", cn);
    }
    if (uint8_t* a = ReflAddr(tmpl, N_LeftButtonVerticalSize, true)) {
        float f = 0;
        if (Rd(a, f)) Out("LeftButtonVerticalSize=%f", (double)f);
    }
    DumpArray(tmpl, N_SelectorTabs, ArrKind::Count);
    DumpArray(tmpl, N_LeftButtons, ArrKind::Text);
    DumpArray(tmpl, N_AltLeftButtonText, ArrKind::Text);
    DumpArray(tmpl, N_FunctionToCallOnPress, ArrKind::Str);
    Out("== end of template");
}

// Logs the first (lowest-offset) StrProperty parameter of func, read from parms.
static void LogFirstStrParm(void* func, const char* cls, const char* fnName, uint8_t* parms) {
    int32_t best = -1;
    uint8_t* p = nullptr;
    Rd((uint8_t*)func + UE::Struct_PropLink, p);
    for (int i = 0; p && i < 256; ++i) {
        uint64_t flags = 0;
        int32_t off = 0;
        if (Rd(p + UE::Prop_Flags, flags) && (flags & UE::CPF_Parm) && !(flags & UE::CPF_ReturnParm) &&
            Rd(p + UE::Prop_Offset, off) && off >= 0 && (best < 0 || off < best)) {
            char t[64];
            FieldTypeName(p, t, sizeof t);
            if (strcmp(t, "StrProperty") == 0) best = off;
        }
        if (!Rd(p + UE::Prop_NextLink, p)) break;
    }
    if (best < 0 || !parms) { Out("%s::%s no FString param", cls, fnName); return; }
    char s[1024];
    if (!ReadFString(parms + best, s, sizeof s)) snprintf(s, sizeof s, "<unreadable>");
    Out("%s::%s param0 FString=\"%s\"", cls, fnName, s);
}

// Logs Gameterface (UUI_Menu_Base) state: null, or its name, class and liveness.
static void LogGameterface(void* obj) {
    uint8_t* a = ReflAddr(obj, N_Gameterface, true);
    void* g = nullptr;
    if (!a || !Rd(a, g)) return;
    if (!g) { Out("  Gameterface=null"); return; }
    char gn[256], gc[256];
    ObjName(g, gn, sizeof gn);
    ClassName(g, gc, sizeof gc);
    Out("  Gameterface %s class=%s @%p live=%d", gn, gc, g, (int)IsLive(g));
}

// Logs the first 32 raw bytes of InteractedButton parms as hex.
static void LogParmsHex(uint8_t* parms) {
    uint8_t b[32];
    if (!parms || !gtade::SafeRead(parms, b, sizeof b)) { Out("InteractedButton parms unreadable"); return; }
    char hex[32 * 3 + 1];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 3, 4, "%02x ", b[i]);
    Out("InteractedButton parms[0..32)= %s", hex);
}

// Widget tree node: class, name, text for TextBlock, then Slots -> Content recursion.
static void DumpNode(void* w, int depth) {
    if (!w) { Out("%*s(null)", depth * 2, ""); return; }
    if (depth >= kMaxDepth) { Out("%*s... (depth cap)", depth * 2, ""); return; }
    if (!IsLive(w)) { Out("%*s<not a live UObject @%p>", depth * 2, "", w); return; }
    char cn[256], nm[256], text[1100] = "";
    ClassName(w, cn, sizeof cn);
    ObjName(w, nm, sizeof nm);
    if (IsA(w, "TextBlock")) {
        char t[1024];
        uint8_t* a = ReflAddr(w, N_Text, true);
        if (a && TextToUtf8(a, t, sizeof t)) snprintf(text, sizeof text, " text=\"%s\"", t);
    }
    Out("%*s%s '%s' @%p%s", depth * 2, "", cn, nm, w, text);

    uint8_t* slots = ReflAddr(w, N_Slots, false);  // leaf widgets have no Slots
    void* data = nullptr;
    int32_t num = 0;
    if (!slots || !Rd(slots, data) || !Rd(slots + 8, num) || !data || num <= 0) return;
    if (num > kMaxSlots) num = kMaxSlots;
    for (int i = 0; i < num; ++i) {
        void* slot = nullptr;
        if (!Rd((uint8_t*)data + (size_t)i * 8, slot) || !slot || !IsLive(slot)) continue;
        Out("%*sslot[%d]", (depth + 1) * 2, "", i);
        DumpNode(PropObject(slot, N_Content), depth + 2);
    }
}

static void DumpWidgetOnce(void* obj, void* cls) {
    static void* dumped[64];
    static int dumpedCount;
    for (int i = 0; i < dumpedCount; ++i)
        if (dumped[i] == cls) return;
    if (dumpedCount >= 64) return;
    dumped[dumpedCount++] = cls;

    char cn[256];
    ObjName(cls, cn, sizeof cn);
    Out("== widget tree of %s @%p", cn, obj);
    void* tree = PropObject(obj, N_WidgetTree);
    if (!tree) { Out("  no WidgetTree"); return; }
    DumpNode(PropObject(tree, N_RootWidget), 0);
    Out("== end of tree");
}

// Appends to out (NUL-terminated, capacity cap).
static void Cat(char* out, size_t cap, const char* fmt, ...) {
    size_t n = strlen(out);
    if (n >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
}

// True once per c in set; false if c is already present or the set is full.
static bool MarkOnce(void** set, int& n, int cap, void* c) {
    for (int i = 0; i < n; ++i)
        if (set[i] == c) return false;
    if (n >= cap) return false;
    set[n++] = c;
    return true;
}

static void* g_clsMarks[kMaxClassMarks];
static int g_clsMarkN;
static void* g_instMarks[kMaxClassMarks];
static int g_instMarkN;

// UObject pointer as a name: "(null)", the object name, or "<not live>".
static void RefName(void* c, char* out, size_t cap) {
    if (!c) snprintf(out, cap, "(null)");
    else if (IsLive(c)) ObjName(c, out, cap);
    else snprintf(out, cap, "<not live @%p>", c);
}

static void FieldName(const uint8_t* f, char* out, size_t cap) {
    int32_t idx = 0;
    if (!Rd(f + UE::FField_Name, idx)) { snprintf(out, cap, "?"); return; }
    NameToStr(idx, out, cap);
}

// Type name plus sub-type: Inner (arrays), PropertyClass (objects), Struct (structs).
static void DescribeProp(const uint8_t* p, char* out, size_t cap, int depth = 0) {
    char ft[64];
    FieldTypeName((uint8_t*)p, ft, sizeof ft);
    snprintf(out, cap, "%s", ft);
    void* sub = nullptr;
    if (depth >= 2 || !Rd(p + UE::Prop_Sub, sub)) return;
    if (strstr(ft, "Array")) {
        if (!sub) return;
        char d[384];
        DescribeProp((const uint8_t*)sub, d, sizeof d, depth + 1);
        Cat(out, cap, " inner=(%s)", d);
    } else if (strstr(ft, "Struct")) {
        char n[256];
        RefName(sub, n, sizeof n);
        Cat(out, cap, " struct=%s", n);
    } else if (strstr(ft, "Object") || strstr(ft, "Class")) {
        char n[256];
        RefName(sub, n, sizeof n);
        Cat(out, cap, " class=%s", n);
    }
}

static void ParmFlags(uint64_t f, char* out, size_t cap) {
    out[0] = 0;
    if (f & UE::CPF_Parm) Cat(out, cap, "%sParm", out[0] ? "|" : "");
    if (f & UE::CPF_OutParm) Cat(out, cap, "%sOutParm", out[0] ? "|" : "");
    if (f & UE::CPF_ReturnParm) Cat(out, cap, "%sReturnParm", out[0] ? "|" : "");
    if (!out[0]) snprintf(out, cap, "-");
}

// Logs each FProperty in cls's ChildProperties (own members only).
static void DumpProps(void* cls, const char* clsName) {
    uint8_t* p = nullptr;
    Rd((uint8_t*)cls + UE::Struct_ChildProps, p);
    for (int i = 0; p && i < kMaxMembers; ++i) {
        char name[256], desc[384];
        FieldName(p, name, sizeof name);
        DescribeProp(p, desc, sizeof desc);
        int32_t off = -1, es = -1;
        Rd(p + UE::Prop_Offset, off);
        Rd(p + UE::Prop_ElemSize, es);
        Out("CLS %s.%s : %s off=0x%x elem=%d", clsName, name, desc, off, es);
        if (!Rd(p + UE::FField_Next, p)) break;
    }
}

// Logs the params of one UFunction (its ChildProperties).
static void DumpParams(void* fn, const char* clsName, const char* fnm) {
    uint8_t* p = nullptr;
    Rd((uint8_t*)fn + UE::Struct_ChildProps, p);
    for (int i = 0; p && i < kMaxMembers; ++i) {
        char name[256], desc[384], fl[64];
        FieldName(p, name, sizeof name);
        DescribeProp(p, desc, sizeof desc);
        uint64_t flags = 0;
        Rd(p + UE::Prop_Flags, flags);
        ParmFlags(flags, fl, sizeof fl);
        Out("CLS %s.%s param %s : %s [%s]", clsName, fnm, name, desc, fl);
        if (!Rd(p + UE::FField_Next, p)) break;
    }
}

// Logs every UFunction in cls's Children chain, with params.
static void DumpFunctions(void* cls, const char* clsName) {
    void* f = nullptr;
    Rd((uint8_t*)cls + UE::Struct_Children, f);
    for (int i = 0; f && i < kMaxMembers; ++i) {
        char cn[256];
        ClassName(f, cn, sizeof cn);
        if (strcmp(cn, "Function") == 0) {
            char fnm[256];
            ObjName(f, fnm, sizeof fnm);
            Out("CLS %s fn %s", clsName, fnm);
            DumpParams(f, clsName, fnm);
        }
        if (!Rd((uint8_t*)f + UE::UField_Next, f)) break;
    }
}

// Dumps cls: super chain, then props and functions of each class up to (not incl.) UUserWidget/UObject.
// Each class body is logged once per process.
static void DumpClass(void* cls) {
    if (!cls || !IsLive(cls) || !MarkOnce(g_clsMarks, g_clsMarkN, kMaxClassMarks, cls)) return;
    char cn[256];
    ObjName(cls, cn, sizeof cn);
    Out("CLS class %s @%p", cn, cls);
    void* s = cls;
    for (int depth = 0; s && depth < kSuperHops && IsLive(s); ++depth) {
        char sn[256];
        ObjName(s, sn, sizeof sn);
        Out("CLS   chain[%d] %s", depth, sn);
        if (strcmp(sn, "UUserWidget") == 0 || strcmp(sn, "UObject") == 0) break;
        if (s == cls || MarkOnce(g_clsMarks, g_clsMarkN, kMaxClassMarks, s)) {
            DumpProps(s, sn);
            DumpFunctions(s, sn);
        }
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
}

// One instance property value, for IntProperty, FloatProperty, BoolProperty, TextProperty,
// StrProperty, ObjectProperty and ClassProperty; other types are skipped.
static void InstValue(uint8_t* obj, const uint8_t* p, const char* clsName) {
    char name[256], ft[64], v[1200] = "";
    FieldName(p, name, sizeof name);
    FieldTypeName((uint8_t*)p, ft, sizeof ft);
    int32_t off = 0;
    if (!Rd(p + UE::Prop_Offset, off) || off < 0 || off > 0x10000) return;
    uint8_t* a = obj + off;
    if (strcmp(ft, "IntProperty") == 0) {
        int32_t x = 0;
        if (Rd(a, x)) snprintf(v, sizeof v, "%d", x);
    } else if (strcmp(ft, "FloatProperty") == 0) {
        float x = 0;
        if (Rd(a, x)) snprintf(v, sizeof v, "%f", (double)x);
    } else if (strcmp(ft, "BoolProperty") == 0) {
        // Bit location read from FBoolProperty ByteOffset (+0x79) and FieldMask (+0x7B): inferred, not runtime-verified.
        uint8_t bo = 0, mask = 0, b = 0;
        if (Rd(p + UE::Prop_BoolByte, bo) && Rd(p + UE::Prop_BoolMask, mask) && Rd(a + bo, b))
            snprintf(v, sizeof v, "%s (byte=0x%02x mask=0x%02x)", (b & mask) ? "true" : "false", b, mask);
    } else if (strcmp(ft, "TextProperty") == 0) {
        char t[1024];
        if (TextToUtf8(a, t, sizeof t)) snprintf(v, sizeof v, "\"%s\"", t);
    } else if (strcmp(ft, "StrProperty") == 0) {
        char s[1024];
        if (ReadFString(a, s, sizeof s)) snprintf(v, sizeof v, "\"%s\"", s);
    } else if (strcmp(ft, "ObjectProperty") == 0 || strcmp(ft, "ClassProperty") == 0) {
        void* c = nullptr;
        if (Rd(a, c)) RefName(c, v, sizeof v);
    } else {
        return;
    }
    if (!v[0]) snprintf(v, sizeof v, "<unreadable>");
    Out("CLS inst %s.%s = %s", clsName, name, v);
}

// Dumps instance values of obj's class chain, once per class.
static void DumpInstanceOnce(void* obj, void* cls) {
    if (!IsLive(obj) || !MarkOnce(g_instMarks, g_instMarkN, kMaxClassMarks, cls)) return;
    char cn[256];
    ObjName(cls, cn, sizeof cn);
    Out("CLS inst == %s @%p", cn, obj);
    void* s = cls;
    for (int depth = 0; s && depth < kSuperHops && IsLive(s); ++depth) {
        char sn[256];
        ObjName(s, sn, sizeof sn);
        if (strcmp(sn, "UUserWidget") == 0 || strcmp(sn, "UObject") == 0) break;
        uint8_t* p = nullptr;
        Rd((uint8_t*)s + UE::Struct_ChildProps, p);
        for (int i = 0; p && i < kMaxMembers; ++i) {
            InstValue((uint8_t*)obj, p, sn);
            if (!Rd(p + UE::FField_Next, p)) break;
        }
        if (!Rd((uint8_t*)s + UE::Struct_Super, s)) break;
    }
}

// Pause template: logs SelectorTabs index -> class, dumps those classes, then the slot classes and LeftButtonTemplate.
static void DumpPauseTemplateClasses(void* tmpl) {
    uint8_t* hdr = ReflAddr(tmpl, N_SelectorTabs, true);
    void* data = nullptr;
    int32_t num = 0;
    void* tabs[kMaxListItems] = {};
    if (hdr && Rd(hdr, data) && Rd(hdr + 8, num) && data && num > 0) {
        if (num > kMaxListItems) num = kMaxListItems;
        for (int i = 0; i < num; ++i) {
            Rd((uint8_t*)data + (size_t)i * 8, tabs[i]);
            char cn[256];
            RefName(tabs[i], cn, sizeof cn);
            Out("CLS SelectorTabs[%d] -> %s", i, cn);
        }
        for (int i = 0; i < num; ++i) DumpClass(tabs[i]);
    }
    const int slots[] = {N_SliderSlotClass, N_SelectionSlotClass, N_BoolSlotClass, N_MessageSlotClass, N_LeftButtonTemplate};
    for (int id : slots) {
        uint8_t* a = ReflAddr(tmpl, id, true);
        void* c = nullptr;
        if (!a || !Rd(a, c)) continue;
        char cn[256];
        RefName(c, cn, sizeof cn);
        Out("CLS template.%s -> %s", kNames[id].str, cn);
        DumpClass(c);
    }
}

static void Observe(void* obj, void* func, void* parms) {
    void* cls = ClassOf(obj);
    int32_t fn = 0;
    if (!cls || !Rd((uint8_t*)func + UE::Obj_Name, fn)) return;
    int32_t clsName = 0;
    if (Rd((uint8_t*)cls + UE::Obj_Name, clsName) && IsName(clsName, N_ScrollingSlotList)) DumpClass(cls);
    if (!MenuLike(cls)) return;

    char cn[256], fnm[256];
    ObjName(cls, cn, sizeof cn);
    ObjName(func, fnm, sizeof fnm);

    if (IsName(fn, N_Construct) && (strcmp(cn, "UI_PauseMenu_C") == 0 || strcmp(cn, "UI_MainMenu_C") == 0)) {
        if (strcmp(cn, "UI_PauseMenu_C") == 0) DumpClass(cls);
        DumpWidgetOnce(obj, cls);
    }

    if (IsName(fn, N_SetTemplate) && IsMenuBase(cls)) {
        void* tmpl = nullptr;
        if (parms && Rd(parms, tmpl) && tmpl && IsLive(tmpl)) {
            DumpTemplate(tmpl);
            char tn[256];
            ObjName(tmpl, tn, sizeof tn);
            if (strcmp(tn, "GTASA_PauseMenu_Mobile") == 0) DumpPauseTemplateClasses(tmpl);
        }
    }

    if (IsName(fn, N_CustomStr) || IsName(fn, N_CustomStrText) || IsName(fn, N_CustomBoolRet)) {
        if (IsMenuBase(cls)) {
            LogFirstStrParm(func, cn, fnm, (uint8_t*)parms);
            LogGameterface(obj);
        }
    }

    if (IsName(fn, N_InteractedButton)) {
        LogParmsHex((uint8_t*)parms);
        if (IsMenuBase(cls)) LogGameterface(obj);
    }
}

// After Construct of a UI_OptionButton_*_Mobile_C has run, dump its instance values once per class.
static void AfterCall(void* obj, void* func) {
    int32_t fn = 0;
    if (!Rd((uint8_t*)func + UE::Obj_Name, fn) || !IsName(fn, N_Construct)) return;
    void* cls = ClassOf(obj);
    if (!cls) return;
    char cn[256];
    ObjName(cls, cn, sizeof cn);
    size_t len = strlen(cn);
    if (strncmp(cn, "UI_OptionButton_", 16) == 0 && len > 7 && strcmp(cn + len - 7, "_Mobile_C") == 0)
        DumpInstanceOnce(obj, cls);
}

static void Hooked_ProcessEvent(void* obj, void* func, void* parms) {
    if (obj && func) Observe(obj, func, parms);
    o_ProcessEvent(obj, func, parms);
    if (obj && func) AfterCall(obj, func);
}


static void* ResolveThread(void*) {
    for (int tries = 0; tries < 600; ++tries) {
        bool all = true;
        for (int i = 0; i < N_COUNT; ++i) {
            if (g_name[i].Index > 0) continue;
            FNameRaw n = {0, 0};
            g_FNameCtor(&n, kNames[i].str, kFNameFind);
            if (n.Index > 0) {
                g_name[i].Number = n.Number;
                __atomic_store_n(&g_name[i].Index, n.Index, __ATOMIC_RELEASE);
            } else {
                all = false;
            }
        }
        if (all) break;
        usleep(500000);
    }
    for (int i = 0; i < N_COUNT; ++i)
        if (g_name[i].Index <= 0) Out("unresolved name: %s", kNames[i].str);
    Out("names resolved");
    return nullptr;
}

static bool Install() {
    void* hUE4 = (void*)aml->GetLibHandle("libUE4.so");
    if (!hUE4) { Out("ABORT: libUE4.so not loaded"); return false; }
    void *pe = nullptr, *ctor = nullptr, *plain = nullptr, *gobj = nullptr, *find = nullptr, *text = nullptr;
    struct Sig { const char* sym; void** out; };
    const Sig sigs[] = {
        {"_ZN7UObject12ProcessEventEP9UFunctionPv", &pe},
        {"_ZN5FNameC1EPKc9EFindName", &ctor},
        {"_ZNK5FName18GetPlainANSIStringERA1024_c", &plain},
        {"GUObjectArray", &gobj},
        {"_ZNK7UStruct18FindPropertyByNameE5FName", &find},
        {"_ZNK5FText8ToStringEv", &text},
    };
    for (const Sig& s : sigs) {
        *s.out = (void*)aml->GetSym(hUE4, s.sym);
        if (!*s.out) { Out("ABORT: missing %s", s.sym); return false; }
    }
    g_FNameCtor = (FNameCtor_Fn)ctor;
    g_FNamePlain = (FNamePlain_Fn)plain;
    g_findProp = (FindProp_Fn)find;
    g_textToString = (TextToString_Fn)text;
    g_objects = (uint8_t*)gobj + 0x10;  // FChunkedFixedUObjectArray lives at GUObjectArray+0x10
    if (!aml->Hook(pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent)) {
        Out("ABORT: ProcessEvent hook failed");
        return false;
    }
    pthread_t th;
    pthread_create(&th, nullptr, ResolveThread, nullptr);
    pthread_detach(th);
    Out("installed");
    return true;
}

ON_MOD_LOAD() {
    Install();
}
