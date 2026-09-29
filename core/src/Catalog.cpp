#include "schwung/Catalog.h"
#include "nlohmann/json.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <map>

using json = nlohmann::json;

namespace schwung {
namespace {

bool isFile(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

std::vector<std::string> listDirs(const std::string& p) {
    std::vector<std::string> out;
    if (DIR* d = ::opendir(p.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string full = p + "/" + e->d_name;
            struct stat st;
            if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(e->d_name);
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// module.json may put capability keys at top level or under "capabilities".
const json* cap(const json& j, const char* k) {
    if (j.contains("capabilities") && j["capabilities"].is_object() && j["capabilities"].contains(k))
        return &j["capabilities"][k];
    if (j.contains(k)) return &j[k];
    return nullptr;
}

} // namespace

void Catalog::scan(const std::string& schwungDir, const std::string& catalogJson) {
    mods_.clear();
    std::map<std::string, json> meta;
    if (!catalogJson.empty()) {
        std::ifstream f(catalogJson);
        json c = json::parse(f, nullptr, false);
        if (c.is_object() && c.contains("modules"))
            for (auto& m : c["modules"]) meta[str(m, "id")] = m;
    }
    // Directory -> the component type the chain host resolves it as.
    static const std::pair<const char*, const char*> kDirs[] = {
        {"sound_generators", "sound_generator"}, {"audio_fx", "audio_fx"},
        {"midi_fx", "midi_fx"}, {"tools", "tool"}, {"overtake", "overtake"},
        {"utilities", "utility"}};
    const std::string root = schwungDir + "/modules";
    for (auto& [dir, type] : kDirs) {
        for (auto& id : listDirs(root + "/" + dir)) {
            const std::string mdir = root + "/" + dir + "/" + id;
            std::ifstream f(mdir + "/module.json");
            if (!f) continue;
            json j = json::parse(f, nullptr, false);
            if (!j.is_object()) continue;
            ModuleInfo m;
            m.id = str(j, "id").empty() ? id : str(j, "id");
            m.name = str(j, "name").empty() ? m.id : str(j, "name");
            m.abbrev = str(j, "abbrev");
            m.description = str(j, "description");
            m.author = str(j, "author");
            m.version = str(j, "version");
            m.componentType = type;
            if (const json* ct = cap(j, "component_type"); ct && ct->is_string()) {
                // Trust the directory for chain loading; keep the declared type
                // only where it refines a non-chain category.
                if (m.componentType != "sound_generator" && m.componentType != "audio_fx" &&
                    m.componentType != "midi_fx") m.componentType = ct->get<std::string>();
            }
            m.dir = mdir;
            const std::string want = (m.componentType == "audio_fx") ? (id + ".so") : "dsp.so";
            m.hasDsp = isFile(mdir + "/" + want);
            m.chainable = m.hasDsp && (m.componentType == "sound_generator" ||
                                       m.componentType == "audio_fx" || m.componentType == "midi_fx");
            auto it = meta.find(m.id);
            if (it != meta.end()) {
                m.subcategory = str(it->second, "subcategory");
                if (it->second.contains("tags"))
                    for (auto& t : it->second["tags"]) if (t.is_string()) m.tags.push_back(t);
                if (m.author.empty()) m.author = str(it->second, "author");
                if (m.description.empty()) m.description = str(it->second, "description");
            }
            mods_.push_back(std::move(m));
        }
    }
    std::sort(mods_.begin(), mods_.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        if (a.componentType != b.componentType) return a.componentType < b.componentType;
        std::string x = a.name, y = b.name;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    });
}

std::vector<ModuleInfo> Catalog::ofType(const std::string& t) const {
    std::vector<ModuleInfo> out;
    for (auto& m : mods_) if (m.componentType == t) out.push_back(m);
    return out;
}

const ModuleInfo* Catalog::find(const std::string& id, const std::string& t) const {
    for (auto& m : mods_) if (m.id == id && (t.empty() || m.componentType == t)) return &m;
    return nullptr;
}

} // namespace schwung
