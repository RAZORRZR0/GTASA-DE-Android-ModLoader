// modmenu_core.cpp - see modmenu_core.h.
#include "modmenu_core.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <set>

namespace modmenu {
namespace {

const std::string kEnabledSuffix = ".so";
const std::string kDisabledSuffix = ".so.disabled";

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Entry names of dir, sorted. Dot entries are skipped.
bool ListDir(const std::string& dir, std::vector<std::string>& names) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (const dirent* e = readdir(d))
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    return true;
}

// "libfoo.so" -> "foo" (enabled); "libfoo.so.disabled" -> "foo" (disabled); anything else, or modmenu itself -> false.
bool ParseModFile(const std::string& file, std::string& name, bool& enabled) {
    std::string base;
    if (EndsWith(file, kDisabledSuffix)) {
        base = file.substr(0, file.size() - kDisabledSuffix.size());
        enabled = false;
    } else if (EndsWith(file, kEnabledSuffix)) {
        base = file.substr(0, file.size() - kEnabledSuffix.size());
        enabled = true;
    } else {
        return false;
    }
    name = base.compare(0, 3, "lib") == 0 && base.size() > 3 ? base.substr(3) : base;
    return !name.empty() && name != "modmenu";  // never list or rename ourselves
}

bool IsCommentLine(const std::string& line) {
    std::string t = Trim(line);
    return !t.empty() && (t[0] == ';' || t[0] == '#');
}

// "N = text" (N decimal, at most 6 digits) -> num, text.
bool ParseLabel(const std::string& body, int& num, std::string& text) {
    size_t d = 0;
    while (d < body.size() && std::isdigit((unsigned char)body[d])) ++d;
    if (d == 0 || d > 6) return false;
    size_t i = d;
    while (i < body.size() && body[i] == ' ') ++i;
    if (i >= body.size() || body[i] != '=') return false;
    num = std::stoi(body.substr(0, d));
    text = Trim(body.substr(i + 1));
    return true;
}

// True if text contains "<digit> =" as a whole number (not "11 =").
bool MentionsOption(const std::string& text, char digit) {
    std::string pat = std::string(1, digit) + " =";
    for (size_t p = text.find(pat); p != std::string::npos; p = text.find(pat, p + 1))
        if (p == 0 || !std::isdigit((unsigned char)text[p - 1])) return true;
    return false;
}

// Comment block directly above line `at`. Fills description and labels of k; mention collects all
// comment text for kind inference. An indented comment line after a label continues that label.
void ReadComments(const std::vector<std::string>& lines, size_t at, IniKey& k, std::string& mention) {
    size_t first = at;
    while (first > 0 && IsCommentLine(lines[first - 1])) --first;
    bool inLabel = false;
    for (size_t j = first; j < at; ++j) {
        const std::string& raw = lines[j];
        std::string after = raw.substr(raw.find_first_of(";#") + 1);
        std::string body = Trim(after);
        mention += body + " ";
        int num = 0;
        std::string text;
        if (ParseLabel(body, num, text)) {
            k.labels.push_back({num, text});
            inLabel = true;
        } else if (inLabel && after.compare(0, 2, "  ") == 0) {
            k.labels.back().text += " " + body;
        } else {
            inLabel = false;
            if (!k.description.empty()) k.description += ' ';
            k.description += body;
        }
    }
}

bool IsIntText(const std::string& s) {
    size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

bool IsFloatText(const std::string& s) {
    size_t i = (!s.empty() && s[0] == '-') ? 1 : 0, digits = 0, dots = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == '.') ++dots;
        else if (std::isdigit((unsigned char)s[i])) ++digits;
        else return false;
    }
    return dots == 1 && digits > 0;
}

// Choice: labels for 3+ distinct integers. Bool: 0/1 value, unless the comment only describes a "0 = ..."
// option (e.g. "0 = stays" on a seconds value). Otherwise Int, Float or String by value format.
void InferKind(IniKey& k, const std::string& mention) {
    std::set<int> values;
    for (const Label& l : k.labels) values.insert(l.value);
    const bool zeroOnly = MentionsOption(mention, '0') && !MentionsOption(mention, '1');
    if (values.size() >= 3) k.kind = ValueKind::Choice;
    else if ((k.value == "0" || k.value == "1") && !zeroOnly) k.kind = ValueKind::Bool;
    else if (IsIntText(k.value)) k.kind = ValueKind::Int;
    else if (IsFloatText(k.value)) k.kind = ValueKind::Float;
    else k.kind = ValueKind::String;
}

std::string IniStem(const std::string& file) {
    return EndsWith(file, ".ini") ? file.substr(0, file.size() - 4) : file;
}

}  // namespace

// ---- Files -------------------------------------------------------------------

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool WriteFileAtomic(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) remove(tmp.c_str());
    return ok;
}

// ---- Mods --------------------------------------------------------------------

bool ModList::Scan(const std::string& dir) {
    std::vector<std::string> names;
    mods_.clear();
    if (!ListDir(dir, names)) return false;
    for (const std::string& f : names) {
        ModEntry m;
        if (!ParseModFile(f, m.name, m.enabled)) continue;
        m.path = dir + "/" + f;
        mods_.push_back(m);
    }
    std::stable_sort(mods_.begin(), mods_.end(),
                     [](const ModEntry& a, const ModEntry& b) { return a.name < b.name; });
    return true;
}

bool ModList::SetEnabled(size_t i, bool enabled) {
    if (i >= mods_.size()) return false;
    ModEntry& m = mods_[i];
    if (m.enabled == enabled) return true;
    const std::string& suffix = m.enabled ? kEnabledSuffix : kDisabledSuffix;
    std::string stem = m.path.substr(0, m.path.size() - suffix.size());
    std::string to = stem + (enabled ? kEnabledSuffix : kDisabledSuffix);
    if (access(to.c_str(), F_OK) == 0) return false;  // rename() would overwrite the other file
    if (rename(m.path.c_str(), to.c_str()) != 0) return false;
    m.path = to;
    m.enabled = enabled;
    m.pendingRestart = true;
    return true;
}

// ---- INI ---------------------------------------------------------------------

bool IniDoc::Load(const std::string& path) {
    std::string text;
    if (!ReadFile(path, text)) return false;
    path_ = path;
    Parse(text);
    return true;
}

void IniDoc::Parse(const std::string& text) {
    lines_.clear();
    keys_.clear();
    for (size_t start = 0;;) {
        size_t nl = text.find('\n', start);
        lines_.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    std::string section;
    for (size_t i = 0; i < lines_.size(); ++i) {
        const std::string& ln = lines_[i];
        std::string t = Trim(ln);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            size_t close = t.find(']');
            if (close != std::string::npos) section = Trim(t.substr(1, close - 1));
            continue;
        }
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        IniKey k;
        k.section = section;
        k.key = Trim(ln.substr(0, eq));
        if (k.key.empty()) continue;
        k.id = section.empty() ? k.key : section + "." + k.key;
        k.line = i;
        size_t vb = eq + 1, ve = ln.size();
        while (vb < ve && (ln[vb] == ' ' || ln[vb] == '\t')) ++vb;
        while (ve > vb && std::isspace((unsigned char)ln[ve - 1])) --ve;
        k.valueBegin = vb;
        k.valueEnd = ve;
        k.value = ln.substr(vb, ve - vb);
        std::string mention;
        ReadComments(lines_, i, k, mention);
        InferKind(k, mention);
        keys_.push_back(k);
    }
}

std::string IniDoc::Text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += '\n';
        out += lines_[i];
    }
    return out;
}

const IniKey* IniDoc::Find(const std::string& id) const {
    for (const IniKey& k : keys_)
        if (k.id == id) return &k;
    return nullptr;
}

bool IniDoc::Set(const std::string& id, const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) return false;
    for (IniKey& k : keys_) {
        if (k.id != id) continue;
        lines_[k.line].replace(k.valueBegin, k.valueEnd - k.valueBegin, value);
        k.valueEnd = k.valueBegin + value.size();
        k.value = value;
        return true;
    }
    return false;
}

bool IniDoc::Save() const {
    return !path_.empty() && WriteFileAtomic(path_, Text());
}

// ---- Links -------------------------------------------------------------------

std::string NormalizeName(const std::string& name) {
    std::string s;
    for (char c : name)
        if (std::isalnum((unsigned char)c)) s += (char)std::tolower((unsigned char)c);
    if (s.size() > 3 && s.compare(0, 3, "lib") == 0) s.erase(0, 3);
    return s;
}

std::map<std::string, std::string> ParseLinks(const IniDoc& doc) {
    std::map<std::string, std::string> out;
    for (const IniKey& k : doc.Keys())
        if (k.section == "Links") out[NormalizeName(k.key)] = k.value;
    return out;
}

std::string FindIni(const std::string& modName, const std::vector<std::string>& iniFiles,
                    const std::map<std::string, std::string>& links) {
    const std::string key = NormalizeName(modName);
    auto it = links.find(key);
    if (it != links.end() && std::find(iniFiles.begin(), iniFiles.end(), it->second) != iniFiles.end())
        return it->second;
    for (const std::string& f : iniFiles)
        if (NormalizeName(IniStem(f)) == key) return f;
    return "";
}

std::vector<std::string> ListConfigs(const std::string& dir) {
    std::vector<std::string> names, out;
    ListDir(dir, names);
    for (const std::string& f : names)
        if (EndsWith(f, ".ini") && f != "modmenu.ini") out.push_back(f);
    return out;
}

std::vector<std::string> UnlinkedInis(const std::vector<std::string>& iniFiles,
                                      const std::vector<std::string>& modNames,
                                      const std::map<std::string, std::string>& links) {
    std::set<std::string> used;
    for (const std::string& m : modNames) {
        std::string f = FindIni(m, iniFiles, links);
        if (!f.empty()) used.insert(f);
    }
    std::vector<std::string> out;
    for (const std::string& f : iniFiles)
        if (!used.count(f)) out.push_back(f);
    return out;
}

std::string StepNumber(const std::string& value, ValueKind kind, int dir) {
    char* end = nullptr;
    const double v = strtod(value.c_str(), &end);
    if (end == value.c_str()) return value;
    if (kind == ValueKind::Int) {
        long n = (long)v + dir;
        if (v >= 0 && n < 0) n = 0;
        return std::to_string(n);
    }
    if (kind != ValueKind::Float) return value;
    const size_t dot = value.find('.');
    int decimals = dot == std::string::npos ? 1 : (int)(value.size() - dot - 1);
    if (decimals < 1) decimals = 1;
    double n = v + 0.1 * dir;
    if (v >= 0 && n < 0) n = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", decimals, n);
    return buf;
}

std::string Humanize(const std::string& name) {
    std::string out;
    const size_t n = name.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (c == '_') { if (!out.empty() && out.back() != ' ') out += ' '; continue; }
        if (i > 0 && isupper((unsigned char)c) && !out.empty() && out.back() != ' ') {
            const char p = name[i - 1];
            const bool nextLower = i + 1 < n && islower((unsigned char)name[i + 1]);
            if (islower((unsigned char)p) || isdigit((unsigned char)p) || (isupper((unsigned char)p) && nextLower)) out += ' ';
        }
        out += c;
    }
    return out;
}

std::string ModTitle(const std::string& modName, const std::string& iniFile) {
    if (iniFile.empty()) return modName;
    std::string t = iniFile;
    if (t.size() > 4 && t.compare(t.size() - 4, 4, ".ini") == 0) t.resize(t.size() - 4);
    std::replace(t.begin(), t.end(), '_', ' ');
    return t;
}

bool ParseRange(const std::string& description, double& lo, double& hi) {
    std::string d = description;
    std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c) { return (char)tolower(c); });
    const size_t at = d.find("range:");
    if (at == std::string::npos) return false;
    const size_t dots = description.find("..", at + 6);
    if (dots == std::string::npos) return false;
    const std::string first = description.substr(at + 6, dots - (at + 6));  // strtod alone would eat "0." of "0..1"
    char* end = nullptr;
    const double a = strtod(first.c_str(), &end);
    if (end == first.c_str() || strspn(end, " \t") != strlen(end)) return false;
    const char* s = description.c_str() + dots + 2;
    const double b = strtod(s, &end);
    if (end == s || !(a < b)) return false;
    lo = a;
    hi = b;
    return true;
}

std::string FormatNumber(double v, ValueKind kind) {
    char buf[48];
    if (kind == ValueKind::Int) {
        snprintf(buf, sizeof buf, "%lld", (long long)llround(v));
        return buf;
    }
    snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    if (s == "-0.0") s = "0.0";
    return s;
}

}  // namespace modmenu
