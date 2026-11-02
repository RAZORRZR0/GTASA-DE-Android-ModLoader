// Mounts every .pak in Android/data/<game>/paks/ above the game's own paks, so their files replace the game's
// (e.g. GTA Radio Restore's station paks). The paks are the user's own copies: this mod ships none.
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>

MYMOD(net.gtamod.pakloader, PakLoader, 1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

// Above every order the game uses (logged below); the highest-order pak wins a file both paks contain.
static const uint32_t kOrder = 10000;

static std::u16string widen(const std::string &s) { return std::u16string(s.begin(), s.end()); } // paths are ASCII
static std::string narrow(const char16_t *s) { std::string r; while (s && *s) r += (char)*s++; return r; }

static bool mounted;

// bool FPakPlatformFile::Mount(const TCHAR* InPakFilename, uint32 PakOrder, const TCHAR* InPath, bool bLoadIndex)
DECL_HOOK(bool, Mount, void *self, const char16_t *file, uint32_t order, const char16_t *path, bool loadIndex)
{
    bool ok = Mount(self, file, order, path, loadIndex);
    logger->Info("game pak %s order %u: %s", narrow(file).c_str(), order, ok ? "mounted" : "FAILED");
    if (mounted) return ok;
    mounted = true;

    std::string dir = aml->GetAndroidDataRootPath();
    if (!dir.empty() && dir.back() != '/') dir += '/';
    dir += "paks/";
    std::vector<std::string> paks;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pak") == 0) paks.push_back(n);
        }
        closedir(d);
    }
    else logger->Info("no %s", dir.c_str());
    std::sort(paks.begin(), paks.end()); // later names win between user paks
    for (size_t i = 0; i < paks.size(); i++)
    {
        std::u16string p = widen(dir + paks[i]);
        bool r = Mount(self, p.c_str(), kOrder + (uint32_t)i, nullptr, true);
        logger->Info("user pak %s order %u: %s", paks[i].c_str(), kOrder + (uint32_t)i, r ? "mounted" : "FAILED");
    }
    return ok;
}

extern "C" void OnModLoad()
{
    logger->SetTag("PakLoader");
    void *ue4 = aml->GetLibHandle("libUE4.so");
    uintptr_t mount = ue4 ? aml->GetSym(ue4, "_ZN16FPakPlatformFile5MountEPKDsjS1_b") : 0;
    if (!mount) { logger->Error("FPakPlatformFile::Mount not found"); return; }
    HOOK(Mount, mount);
}
