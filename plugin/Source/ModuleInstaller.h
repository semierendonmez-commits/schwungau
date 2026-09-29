// Places the modules shipped inside the plugin bundle where Schwung expects
// them: <dataRoot>/schwung/modules — the desktop equivalent of what the
// Schwung installer does to /data/UserData/schwung on a Move.
#pragma once
#include <juce_core/juce_core.h>

namespace schwung {

struct DataLocation {
    juce::File dataRoot;     // stands in for /data/UserData
    juce::File schwungDir;   // dataRoot/schwung
};

// Where user data lives. SCHWUNG_DATA_ROOT overrides.
DataLocation locateData();

// Copy bundled modules/patches into the data root if the bundle's build id
// differs from the last one installed. User-added modules are never touched.
// Safe to call from several plugin instances: runs once per process.
// Returns a human-readable status line.
juce::String installBundledModules(const DataLocation& loc);

} // namespace schwung
