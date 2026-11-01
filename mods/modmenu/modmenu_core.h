// modmenu_core.h - platform-independent core of the modmenu mod: the AML mod list and INI
// documents with their mod links. Plain C++17 and POSIX only (no UE, no AML), so it builds and
// tests on the host (test_core.cpp). Android API 23 has no std::filesystem, hence dirent/stdio.
#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace modmenu {

// ---- Files -------------------------------------------------------------------
bool ReadFile(const std::string& path, std::string& out);
// Writes text to path + ".tmp", fsyncs, then rename()s over path. Never leaves a half-written file.
bool WriteFileAtomic(const std::string& path, const std::string& text);

// ---- Mods --------------------------------------------------------------------
struct ModEntry {
    std::string name;             // file name without "lib" prefix and ".so" / ".so.disabled"
    std::string path;             // current file: <dir>/x.so or <dir>/x.so.disabled
    bool enabled = false;
    bool pendingRestart = false;  // renamed this session; takes effect at the next launch
};

class ModList {
public:
    // Lists *.so and *.so.disabled in dir, sorted by name. libmodmenu is never listed or renamed.
    bool Scan(const std::string& dir);
    size_t Count() const { return mods_.size(); }
    const ModEntry& At(size_t i) const { return mods_[i]; }
    // Renames x.so <-> x.so.disabled. False if i is out of range, the rename fails, or the target already exists.
    bool SetEnabled(size_t i, bool enabled);

private:
    std::vector<ModEntry> mods_;
};

// ---- INI ---------------------------------------------------------------------
enum class ValueKind { Bool, Choice, Int, Float, String };

struct Label {
    int value;
    std::string text;  // "N = text" comment line (plus indented continuation lines)
};

struct IniKey {
    std::string id;            // "Section.Key", or "Key" outside any section
    std::string section;
    std::string key;
    std::string value;         // raw value text, without surrounding blanks
    std::string description;   // comment block above the key, without the "N = ..." label lines
    std::vector<Label> labels; // every "N = text" comment line of that block, in order
    ValueKind kind = ValueKind::String;
    // Position of the value text on its line. Set() rewrites exactly this range.
    size_t line = 0, valueBegin = 0, valueEnd = 0;
};

class IniDoc {
public:
    bool Load(const std::string& path);
    // Keeps every line (comments, blanks, order). Text() returns the same bytes until Set().
    void Parse(const std::string& text);
    std::string Text() const;
    const std::vector<IniKey>& Keys() const { return keys_; }
    const IniKey* Find(const std::string& id) const;
    // Replaces only the value text of key id. False if no such key or value has a line break.
    bool Set(const std::string& id, const std::string& value);
    // Atomic write back to the path given to Load().
    bool Save() const;

private:
    std::string path_;
    std::vector<std::string> lines_;
    std::vector<IniKey> keys_;
};

// ---- Links -------------------------------------------------------------------
// Match key: lowercase, [a-z0-9] only, leading "lib" dropped. "libpropfix" and "PropFix" both give "propfix".
// One arrow step of a number key: Int by 1, Float by 0.1 keeping its decimals (at least one).
// Values that start non-negative never go below 0 (counts, seconds, percents). Unchanged text on bad input.
std::string StepNumber(const std::string& value, ValueKind kind, int dir);

std::string NormalizeName(const std::string& name);
// [Links] section of configs/modmenu.ini: normalized mod name -> ini file name.
std::map<std::string, std::string> ParseLinks(const IniDoc& doc);
// Ini file for a mod: its explicit [Links] entry if that file exists, else the ini whose normalized
// name matches. "" if none.
std::string FindIni(const std::string& modName, const std::vector<std::string>& iniFiles,
                    const std::map<std::string, std::string>& links);
// *.ini file names in dir, sorted, without modmenu.ini itself.
std::vector<std::string> ListConfigs(const std::string& dir);
// Ini files that no mod in modNames is linked to.
std::vector<std::string> UnlinkedInis(const std::vector<std::string>& iniFiles,
                                      const std::vector<std::string>& modNames,
                                      const std::map<std::string, std::string>& links);

// ---- Display -----------------------------------------------------------------
// Ini name as a label: words split at case changes and underscores, acronyms kept together.
// "GameTimecycStrength" -> "Game Timecyc Strength", "PS2Filter" -> "PS2 Filter", "DEBloom" -> "DE Bloom".
std::string Humanize(const std::string& name);
// Mod title: its ini file name without ".ini", underscores as spaces ("GTA_Prop_Fix.ini" -> "GTA Prop Fix"),
// else the mod name.
std::string ModTitle(const std::string& modName, const std::string& iniFile);
// Slider range from a "range: A..B" comment line in a key's comment block. False if absent or not A < B.
bool ParseRange(const std::string& description, double& lo, double& hi);
// A slider value as ini text: Int rounded, Float with 2 decimals, trailing zeros trimmed to one ("1.50" -> "1.5").
std::string FormatNumber(double v, ValueKind kind);

}  // namespace modmenu
