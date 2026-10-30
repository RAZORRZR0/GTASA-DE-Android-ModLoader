// test_core.cpp - host test for modmenu_core.
// Build: c++ -std=c++17 -Wall -Wextra -o test_core test_core.cpp modmenu_core.cpp && ./test_core [ini]
// Exit 0 = all checks pass.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "modmenu_core.h"

using namespace modmenu;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_pass;                                                            \
        } else {                                                                 \
            ++g_fail;                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

static const char* kPropfixIni = "/tmp/gtamod/repo/mods/propfix/GTA_Prop_Fix.ini";
static const char* kShippedLinks = "/tmp/gtamod/repo/mods/modmenu/configs/modmenu.ini";

static std::string MakeTempDir() {
    char tmpl[] = "/tmp/modmenu_test.XXXXXX";
    char* d = mkdtemp(tmpl);
    return d ? d : "";
}

static void Touch(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) fclose(f);
}

static bool Exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

static void RemoveDir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d))
            if (e->d_name[0] != '.') remove((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    rmdir(dir.c_str());
}

static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t nl = s.find('\n', start);
        out.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

static void TestIni(const std::string& src) {
    std::string dir = MakeTempDir();
    std::string copy = dir + "/GTA_Prop_Fix.ini";
    CHECK(WriteFileAtomic(copy, src));  // edits go to a copy; the repo ini is never written

    IniDoc doc;
    CHECK(doc.Load(copy));
    CHECK(doc.Text() == src);  // every line kept
    CHECK(doc.Keys().size() == 20);

    const IniKey* sl = doc.Find("Props.StreetLightsFallWhole");
    CHECK(sl && sl->kind == ValueKind::Bool);
    const IniKey* rw = doc.Find("Props.RenderWarePhysicsForDislodgedProps");
    CHECK(rw && rw->kind == ValueKind::Bool);
    const IniKey* kp = doc.Find("Props.KnockedPropsDisappearSeconds");  // value 0, comment "0 = stays"
    CHECK(kp && kp->kind == ValueKind::Int);
    const IniKey* lt = doc.Find("Debris.Lifetime");
    CHECK(lt && lt->kind == ValueKind::Choice);
    if (lt) {
        CHECK(lt->labels.size() == 3);
        CHECK(lt->labels.size() == 3 && lt->labels[0].value == 0 && lt->labels[1].value == 1 &&
              lt->labels[2].value == 2);
        CHECK(lt->labels.size() == 3 && !lt->labels[0].text.empty() && !lt->labels[2].text.empty());
        CHECK(lt->description.empty());  // every comment line of Lifetime is a label
    }
    const IniKey* pf = doc.Find("Debris.PiecesFadeSeconds");
    CHECK(pf && pf->kind == ValueKind::Float && pf->value == "2.55");
    const IniKey* log = doc.Find("Debug.Log");
    CHECK(log && log->kind == ValueKind::Bool);
    const IniKey* gs = doc.Find("Props.GenerateCollisionSpheres");
    CHECK(gs && gs->kind == ValueKind::Bool && gs->description.find("SPHERES") != std::string::npos);

    // Set + Save + reparse: only the Lifetime line changes.
    CHECK(doc.Set("Debris.Lifetime", "1"));
    CHECK(doc.Save());
    std::string after;
    CHECK(ReadFile(copy, after));
    std::vector<std::string> a = SplitLines(src), b = SplitLines(after);
    CHECK(a.size() == b.size());
    int diffs = 0;
    size_t where = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) {
            ++diffs;
            where = i;
        }
    CHECK(diffs == 1);
    CHECK(where < a.size() && a[where] == "Lifetime=0" && b[where] == "Lifetime=1");

    IniDoc re;
    CHECK(re.Load(copy));
    CHECK(re.Find("Debris.Lifetime") && re.Find("Debris.Lifetime")->value == "1");
    CHECK(re.Text() == after);

    // Restore the value: the file is byte-identical to the original again.
    CHECK(re.Set("Debris.Lifetime", "0"));
    CHECK(re.Save());
    std::string restored;
    CHECK(ReadFile(copy, restored) && restored == src);
    CHECK(!re.Set("Debris.NoSuchKey", "1"));

    RemoveDir(dir);
}

static void TestMods() {
    std::string dir = MakeTempDir();
    Touch(dir + "/libfoo.so");
    Touch(dir + "/libbar.so.disabled");
    Touch(dir + "/libmodmenu.so");
    Touch(dir + "/libmodmenu.so.disabled");
    Touch(dir + "/notes.txt");

    ModList ml;
    CHECK(ml.Scan(dir));
    CHECK(ml.Count() == 2);
    if (ml.Count() == 2) {
        CHECK(ml.At(0).name == "bar" && !ml.At(0).enabled);
        CHECK(ml.At(1).name == "foo" && ml.At(1).enabled);
        CHECK(!ml.At(0).pendingRestart);
        CHECK(ml.SetEnabled(0, true));
        CHECK(Exists(dir + "/libbar.so") && !Exists(dir + "/libbar.so.disabled"));
        CHECK(ml.At(0).enabled && ml.At(0).pendingRestart);
        CHECK(ml.SetEnabled(1, false));
        CHECK(Exists(dir + "/libfoo.so.disabled") && !Exists(dir + "/libfoo.so"));
        CHECK(!ml.At(1).enabled && ml.At(1).pendingRestart);
        CHECK(ml.SetEnabled(1, false));  // already in that state
    }
    CHECK(!ml.SetEnabled(5, true));
    CHECK(Exists(dir + "/libmodmenu.so") && Exists(dir + "/libmodmenu.so.disabled"));  // untouched

    // Collision: both x.so and x.so.disabled exist. Enabling must not overwrite the enabled file.
    std::string dir2 = MakeTempDir();
    Touch(dir2 + "/libx.so");
    Touch(dir2 + "/libx.so.disabled");
    ModList col;
    CHECK(col.Scan(dir2) && col.Count() == 2);
    size_t off = col.Count();
    for (size_t i = 0; i < col.Count(); ++i)
        if (!col.At(i).enabled) off = i;
    CHECK(off < col.Count());
    if (off < col.Count()) CHECK(!col.SetEnabled(off, true));
    CHECK(Exists(dir2 + "/libx.so") && Exists(dir2 + "/libx.so.disabled"));

    RemoveDir(dir);
    RemoveDir(dir2);
}

static void TestLinks(const std::string& src) {
    std::string dir = MakeTempDir();
    CHECK(WriteFileAtomic(dir + "/GTA_Prop_Fix.ini", src));
    Touch(dir + "/Other.ini");
    CHECK(WriteFileAtomic(dir + "/modmenu.ini", "[Links]\npropfix=GTA_Prop_Fix.ini\n"));

    std::vector<std::string> inis = ListConfigs(dir);
    CHECK(inis.size() == 2 && inis[0] == "GTA_Prop_Fix.ini" && inis[1] == "Other.ini");

    IniDoc links;
    CHECK(links.Load(dir + "/modmenu.ini"));
    auto map = ParseLinks(links);
    CHECK(map.size() == 1);
    CHECK(FindIni("propfix", inis, map) == "GTA_Prop_Fix.ini");     // explicit
    CHECK(FindIni("GTA_Prop_Fix", inis, {}) == "GTA_Prop_Fix.ini");  // by normalized name
    CHECK(FindIni("propfix", inis, {}).empty());                    // no link, no match
    CHECK(NormalizeName("libPropFix") == "propfix");
    CHECK(FindIni("propfix", {"Other.ini"}, map).empty());          // explicit target missing
    std::vector<std::string> unlinked = UnlinkedInis(inis, {"propfix"}, map);
    CHECK(unlinked.size() == 1 && unlinked[0] == "Other.ini");

    // The shipped configs/modmenu.ini links propfix to GTA_Prop_Fix.ini.
    IniDoc shipped;
    CHECK(shipped.Load(kShippedLinks));
    auto sm = ParseLinks(shipped);
    CHECK(sm.count("propfix") && sm["propfix"] == "GTA_Prop_Fix.ini");

    RemoveDir(dir);
}

int main(int argc, char** argv) {
    std::string src;
    const char* path = argc > 1 ? argv[1] : kPropfixIni;
    if (!ReadFile(path, src)) {
        printf("cannot read %s\n", path);
        return 1;
    }
    TestIni(src);
    TestMods();
    TestLinks(src);
    // Number stepping.
    CHECK(StepNumber("60", ValueKind::Int, 1) == "61");
    CHECK(StepNumber("0", ValueKind::Int, -1) == "0");
    CHECK(StepNumber("-3", ValueKind::Int, -1) == "-4");
    CHECK(StepNumber("2.55", ValueKind::Float, 1) == "2.65");
    CHECK(StepNumber("1.5", ValueKind::Float, -1) == "1.4");
    CHECK(StepNumber("0.05", ValueKind::Float, -1) == "0.00");
    CHECK(StepNumber("abc", ValueKind::Float, 1) == "abc");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
