// ProjectPropsDE for Android: Project Props 4 placements in GTA SA: The Definitive Edition (libUE4.so v1.87.0).
// Port of the PC ProjectPropsDE (pak + IplActorsDE + DE_LimitAdjuster pools), in one AML mod:
//  1. Placements: CFileMgr::OpenFile reads a whole file into memory; for each DE IPL named in ProjectPropsDE.dat we
//     swap that buffer for a copy with the lines added at the end of its inst section (the PC pak's edited IPLs).
//  2. IplActorsDE: LoadObjectInstance marks every IPL instance to link to a cooked AIPLMapActor (entity +0x34 bit 3),
//     which new placements never get. Lines without an explicit 12th-column index (ours use -1) get the bit cleared,
//     so CEntity::CreateRwObject spawns a runtime actor for them, as for script objects.
//  3. Pools: CPools::Initialise builds empty pools; configured ones get bigger arrays. MatrixList via
//     CPlaceable::InitMatrixArray (Android inlines Init there).
// Notes: README.md (this folder) and the PC repo's README (reverse-engineering notes).
#include <android/log.h>
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <mod/amlmod.h>

MYMOD(net.gtamod.projectpropsde, ProjectPropsDE, 1.0.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

static int g_log = 1;
#define LOG(...) do { if (g_log) __android_log_print(ANDROID_LOG_INFO, "ProjectPropsDE", __VA_ARGS__); } while (0)

// ---------------------------------------------------------------- config
struct PoolCfg {
    const char* key;
    const char* sym;     // CPools::ms_pXxxPool
    uint32_t elemSize;   // checked against CPools::Initialise's allocation sizes
    int32_t deDefault;
    int32_t wanted;
};
static PoolCfg g_pools[] = {
    { "PtrNodeSingle", "_ZN6CPools25ms_pPtrNodeSingleLinkPoolE", 16, 75000, 0 },
    { "PtrNodeDouble", "_ZN6CPools25ms_pPtrNodeDoubleLinkPoolE", 24, 7500, 0 },
    { "EntryInfoNode", "_ZN6CPools21ms_pEntryInfoNodePoolE", 40, 600, 0 },
    { "Buildings", "_ZN6CPools16ms_pBuildingPoolE", 112, 14000, 0 },
    { "Dummys", "_ZN6CPools13ms_pDummyPoolE", 120, 8500, 0 },
    { "ColModel", "_ZN6CPools16ms_pColModelPoolE", 56, 10400, 0 },
};
constexpr int kMatrixDefault = 1800;
static int g_matrixList = 0;
static bool g_enabled = true;

// Minimal ini read: value of key in [section], or def. Comment lines start with ';'.
static int IniInt(const std::string& ini, const char* section, const char* key, int def) {
    bool in = false;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        std::string l = ini.substr(pos, end - pos);
        pos = end + 1;
        while (!l.empty() && isspace((unsigned char)l.back())) l.pop_back();
        size_t s = l.find_first_not_of(" \t");
        if (s == std::string::npos || l[s] == ';') continue;
        l = l.substr(s);
        if (l[0] == '[') { in = strcasecmp(l.c_str(), (std::string("[") + section + "]").c_str()) == 0; continue; }
        size_t eq = l.find('=');
        if (!in || eq == std::string::npos) continue;
        std::string k = l.substr(0, eq);
        while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
        if (strcasecmp(k.c_str(), key) == 0) return atoi(l.c_str() + eq + 1);
    }
    return def;
}

static bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- placements (ProjectPropsDE.dat)
struct Host {
    std::string path;    // lower case, '/' separators, relative to data/ (e.g. "maps/la/lae.ipl")
    std::string lines;   // "\r\n"-terminated inst lines to add
    int count = 0;
    std::string merged;  // the IPL with our lines, built on first open and kept: the game reads it in place
};
static std::vector<Host> g_hosts;
static long g_total = 0;

static void LoadPlacements(const std::string& path) {
    std::string dat;
    if (!ReadFile(path, dat)) { LOG("%s not found: no props", path.c_str()); return; }
    size_t pos = 0;
    while (pos < dat.size()) {
        size_t end = dat.find('\n', pos);
        if (end == std::string::npos) end = dat.size();
        std::string l = dat.substr(pos, end - pos);
        pos = end + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == ';') continue;
        if (l.compare(0, 5, "host ") == 0) {
            g_hosts.emplace_back();
            g_hosts.back().path = l.substr(5);
            for (char& c : g_hosts.back().path) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
        } else if (!g_hosts.empty()) {
            g_hosts.back().lines += l + "\r\n";
            ++g_hosts.back().count;
            ++g_total;
        }
    }
    LOG("%ld placements for %zu DE IPLs from %s", g_total, g_hosts.size(), path.c_str());
}

static Host* FindHost(const char* name) {
    std::string n(name);
    for (char& c : n) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
    for (Host& h : g_hosts)
        if (n.size() >= h.path.size() && n.compare(n.size() - h.path.size(), h.path.size(), h.path) == 0 &&
            (n.size() == h.path.size() || n[n.size() - h.path.size() - 1] == '/'))
            return &h;
    return nullptr;
}

// Inserts the host's lines before the `end` that closes the inst section. Empty if the IPL has no inst section or
// would exceed LoadScene's 4096 instances per file.
static std::string Merge(const char* data, int size, const Host& h) {
    const std::string text(data, size);
    size_t pos = 0, insertAt = std::string::npos;
    bool inInst = false;
    int stock = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string t = text.substr(pos, end - pos);
        const size_t lineStart = pos;
        pos = end + 1;
        const size_t s = t.find_first_not_of(" \t\r,");
        if (s == std::string::npos || t[s] == '#') continue;
        size_t e = t.find_first_of(" \t\r,", s);
        const std::string tok = t.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (!inInst) {
            inInst = strcasecmp(tok.c_str(), "inst") == 0;
        } else if (strcasecmp(tok.c_str(), "end") == 0) {
            insertAt = lineStart;
            break;
        } else {
            ++stock;
        }
    }
    if (insertAt == std::string::npos) { LOG("%s: no inst section, skipped", h.path.c_str()); return {}; }
    if (stock + h.count > 4096) { LOG("%s: %d + %d instances > 4096, skipped", h.path.c_str(), stock, h.count); return {}; }
    std::string out = text.substr(0, insertAt);
    if (!out.empty() && out.back() != '\n') out += "\r\n";
    out += h.lines + text.substr(insertAt);
    LOG("%s: %d stock + %d props", h.path.c_str(), stock, h.count);
    return out;
}

// CFileMgr's file object (OpenFile reads the whole file): +0 IFileHandle*, +8 TArray<uint8> data (ptr, num, max),
// +0x18 read position. CloseFile frees data through GMalloc, so the original array is put back before it runs.
struct GameFile {
    void* handle;
    char* data;
    int32_t num, max;
    int32_t pos;
};
struct Swap { GameFile* file; char* data; int32_t num, max; };
static std::vector<Swap> g_swaps;
static pthread_mutex_t g_swapLock = PTHREAD_MUTEX_INITIALIZER;

static GameFile* (*o_OpenFile)(const char*, const char*);
static GameFile* H_OpenFile(const char* name, const char* mode) {
    GameFile* f = o_OpenFile(name, mode);
    Host* h = f && name && g_enabled ? FindHost(name) : nullptr;
    if (!h || !f->data || f->pos != 0) return f;
    if (h->merged.empty()) h->merged = Merge(f->data, f->num, *h);
    if (h->merged.empty()) return f;
    pthread_mutex_lock(&g_swapLock);
    g_swaps.push_back({ f, f->data, f->num, f->max });
    f->data = &h->merged[0];
    f->num = f->max = (int32_t)h->merged.size();
    pthread_mutex_unlock(&g_swapLock);
    return f;
}

static int (*o_CloseFile)(GameFile*);
static int H_CloseFile(GameFile* f) {
    pthread_mutex_lock(&g_swapLock);
    for (size_t i = 0; i < g_swaps.size(); ++i)
        if (g_swaps[i].file == f) {
            f->data = g_swaps[i].data;
            f->num = g_swaps[i].num;
            f->max = g_swaps[i].max;
            g_swaps.erase(g_swaps.begin() + i);
            break;
        }
    pthread_mutex_unlock(&g_swapLock);
    return o_CloseFile(f);
}

// ---------------------------------------------------------------- IplActorsDE
// CFileLoader::LoadObjectInstance(CFileObjectInstance*, const char*, bool fromIpl). LoadScene packs an explicit
// 12th-column index into the high word of the struct's model id (+0x1c) as index+1; -1 leaves it 0.
// fromIpl sets entity +0x34 bit 3 (link to a cooked IPL actor).
static long g_marked = 0;
static std::vector<uint8_t*> g_markedEntities;  // Log=2 only: for the actor count below
static uint8_t* (*o_LoadObjectInstance)(uint8_t*, const char*, bool);
static uint8_t* H_LoadObjectInstance(uint8_t* inst, const char* line, bool fromIpl) {
    uint8_t* e = o_LoadObjectInstance(inst, line, fromIpl);
    if (e && fromIpl && (*(uint32_t*)(inst + 0x1c) >> 16) == 0) {
        e[0x34] &= ~0x08;
        if (g_markedEntities.size() < g_markedEntities.capacity()) g_markedEntities.push_back(e);  // never reallocates
        if (++g_marked % 1000 == 1) LOG("runtime actors: %ld placements so far", g_marked);
    }
    return e;
}

// Log=2: every 10 s, how many marked entities exist and how many got a UE actor (+0x20), i.e. are drawn.
// Map entities are never freed, so the pointers stay valid.
static void* StatusThread(void*) {
    for (;;) {
        sleep(10);
        long actors = 0;
        for (uint8_t* e : g_markedEntities) actors += *(void**)(e + 0x20) != nullptr;
        LOG("status: marked=%ld actors=%ld", g_marked, actors);
    }
    return nullptr;
}

// ---------------------------------------------------------------- pools
struct CPool { uint8_t* objects; uint8_t* flags; int32_t size; int32_t firstFree; bool owns; };
static void*** g_gmalloc;  // FMalloc** ; Malloc is vtable slot 3 (+0x18) on arm64, as CPools::Initialise calls it
static CPool** g_poolGlobals[sizeof g_pools / sizeof g_pools[0]];

static void* GameMalloc(size_t bytes) {
    void** gm = *g_gmalloc;
    using MallocFn = void* (*)(void*, size_t, uint32_t);
    return ((MallocFn*)*gm)[3](gm, bytes, 0);
}

// Swaps a freshly built (empty) pool's arrays for bigger ones. DE's arrays are leaked (a few MB, once).
static void Grow(PoolCfg& p, CPool* pool) {
    if (!pool) { LOG("%s: not created", p.key); return; }
    if (pool->size != p.deDefault) { LOG("%s: size %d, expected %d: skipped", p.key, pool->size, p.deDefault); return; }
    if (p.wanted <= pool->size) return;
    uint8_t* objects = (uint8_t*)GameMalloc((size_t)p.wanted * p.elemSize);
    uint8_t* flags = (uint8_t*)GameMalloc((size_t)p.wanted);
    if (!objects || !flags) { LOG("%s: allocation of %d failed, kept %d", p.key, p.wanted, pool->size); return; }
    memset(flags, 0x80, p.wanted);  // free, id 0 (as DE's init loop)
    LOG("%s: %d -> %d", p.key, pool->size, p.wanted);
    pool->objects = objects;
    pool->flags = flags;
    pool->size = p.wanted;
    pool->firstFree = -1;
}

static void (*o_Initialise)();
static void H_Initialise() {
    o_Initialise();
    for (size_t i = 0; i < sizeof g_pools / sizeof g_pools[0]; ++i) Grow(g_pools[i], *g_poolGlobals[i]);
}

// Android inlines CMatrixLinkList::Init(1800) into InitMatrixArray; replace it with the out-of-line Init.
static void (*o_MatrixInit)(void*, int);
static void* g_matrixListObj;
static void H_InitMatrixArray() {
    LOG("MatrixList: %d -> %d", kMatrixDefault, g_matrixList);
    o_MatrixInit(g_matrixListObj, g_matrixList);
}

// ---------------------------------------------------------------- entry
ON_MOD_LOAD() {
    const std::string dir = aml->GetConfigPath();
    const std::string base = dir + (dir.empty() || dir.back() == '/' ? "" : "/");
    std::string ini;
    const bool found = ReadFile(base + "ProjectPropsDE.ini", ini);
    g_log = IniInt(ini, "General", "Log", 1);
    g_enabled = IniInt(ini, "General", "Enabled", 1) != 0;
    for (PoolCfg& p : g_pools) p.wanted = IniInt(ini, "Pools", p.key, 0);
    g_matrixList = IniInt(ini, "Pools", "MatrixList", 0);
    LOG("ProjectPropsDE 1.0.0 | ini %s%s | props %s", base.c_str(), found ? "ProjectPropsDE.ini" : " (ProjectPropsDE.ini not found: defaults)",
        g_enabled ? "on" : "off");
    if (g_enabled) LoadPlacements(base + "ProjectPropsDE.dat");

    void* ue4 = (void*)aml->GetLibHandle("libUE4.so");
    if (!ue4) { LOG("ABORT: libUE4.so not loaded"); return; }
    auto sym = [&](const char* s) { return (void*)aml->GetSym(ue4, s); };
    void* open = sym("_ZN8CFileMgr8OpenFileEPKcS1_");
    void* close = sym("_ZN8CFileMgr9CloseFileEy");
    void* loi = sym("_ZN11CFileLoader18LoadObjectInstanceEPN10SanAndreas19CFileObjectInstanceEPKcb");
    void* init = sym("_ZN6CPools10InitialiseEv");
    void* matrix = sym("_ZN10CPlaceable15InitMatrixArrayEv");
    o_MatrixInit = (void (*)(void*, int))sym("_ZN15CMatrixLinkList4InitEi");
    g_matrixListObj = sym("gMatrixList");
    g_gmalloc = (void***)sym("GMalloc");
    bool ok = open && close && loi && init && matrix && o_MatrixInit && g_matrixListObj && g_gmalloc;
    for (size_t i = 0; i < sizeof g_pools / sizeof g_pools[0]; ++i) ok &= (g_poolGlobals[i] = (CPool**)sym(g_pools[i].sym)) != nullptr;
    if (!ok) { LOG("ABORT: a libUE4.so symbol is missing (unsupported game version): nothing installed"); return; }

    if (*g_poolGlobals[0]) LOG("pools already exist: loaded after CPools::Initialise, pool sizes unchanged");
    else aml->Hook(init, (void*)&H_Initialise, (void**)&o_Initialise);
    if (g_matrixList > kMatrixDefault) aml->Hook(matrix, (void*)&H_InitMatrixArray);
    if (g_total) {
        aml->Hook(open, (void*)&H_OpenFile, (void**)&o_OpenFile);
        aml->Hook(close, (void*)&H_CloseFile, (void**)&o_CloseFile);
    }
    aml->Hook(loi, (void*)&H_LoadObjectInstance, (void**)&o_LoadObjectInstance);  // any map mod using the -1 convention
    LOG("hooks installed");
    if (g_log >= 2) {
        g_markedEntities.reserve(g_total + 4096);  // the status thread reads it while the map loads
        pthread_t t;
        if (pthread_create(&t, nullptr, StatusThread, nullptr) == 0) pthread_detach(t);
    }
}
