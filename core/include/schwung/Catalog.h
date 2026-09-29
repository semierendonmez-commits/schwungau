// Installed-module catalog: what the shell can offer in each chain position.
#pragma once
#include <string>
#include <vector>

namespace schwung {

struct ModuleInfo {
    std::string id, name, abbrev, description, author, version;
    std::string componentType;   // sound_generator | audio_fx | midi_fx | tool | overtake | utility
    std::string subcategory;     // from module-catalog.json (e.g. "polysynth", "reverb")
    std::vector<std::string> tags;
    std::string dir;             // absolute module directory
    bool hasDsp = false;         // a loadable binary is present
    bool chainable = false;      // can be loaded into a Signal Chain position
};

class Catalog {
public:
    // schwungDir: <dataRoot>/schwung. catalogJson: optional path to
    // module-catalog.json for subcategories/tags.
    void scan(const std::string& schwungDir, const std::string& catalogJson = {});
    const std::vector<ModuleInfo>& all() const { return mods_; }
    std::vector<ModuleInfo> ofType(const std::string& componentType) const;
    const ModuleInfo* find(const std::string& id, const std::string& componentType = {}) const;
private:
    std::vector<ModuleInfo> mods_;
};

} // namespace schwung
