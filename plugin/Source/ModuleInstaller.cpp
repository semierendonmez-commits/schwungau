#include "ModuleInstaller.h"
#include <mutex>

namespace schwung {

DataLocation locateData() {
    DataLocation d;
    if (auto* env = std::getenv("SCHWUNG_DATA_ROOT"); env && *env) {
        d.dataRoot = juce::File(juce::String::fromUTF8(env));
    } else {
       #if JUCE_MAC || JUCE_IOS
        // In a sandboxed AUv3 this resolves inside the extension's container.
        d.dataRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("Application Support/Schwung/UserData");
       #elif JUCE_WINDOWS
        d.dataRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("Schwung/UserData");
       #else
        d.dataRoot = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                         .getChildFile(".local/share/Schwung/UserData");
       #endif
    }
    d.schwungDir = d.dataRoot.getChildFile("schwung");
    return d;
}

static juce::File findBundledData() {
    // .../Contents/MacOS/<bin> -> .../Contents/Resources/SchwungData
    // .../Contents/x86_64-linux/<bin>.so -> .../Contents/Resources/SchwungData
    // standalone: <exe dir>/SchwungData
    const juce::File me = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::File candidates[] = {
        me.getParentDirectory().getSiblingFile("Resources").getChildFile("SchwungData"),
        me.getParentDirectory().getChildFile("SchwungData"),
        me.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("SchwungData"),
    };
    for (auto& c : candidates)
        if (c.getChildFile("schwung/modules/chain").isDirectory()) return c;
    return {};
}

static int copyTree(const juce::File& src, const juce::File& dst) {
    int n = 0;
    dst.createDirectory();
    for (const auto& e : juce::RangedDirectoryIterator(src, false, "*", juce::File::findFilesAndDirectories)) {
        const auto& f = e.getFile();
        const auto target = dst.getChildFile(f.getFileName());
        if (f.isDirectory()) { n += copyTree(f, target); continue; }
        if (target.existsAsFile() && target.getSize() == f.getSize() &&
            target.getLastModificationTime() >= f.getLastModificationTime()) continue;
        if (f.copyFileTo(target)) {
            ++n;
           #if ! JUCE_WINDOWS
            if (f.hasFileExtension(".so") || f.hasFileExtension(".dylib"))
                target.setExecutePermission(true);
           #endif
        }
    }
    return n;
}

juce::String installBundledModules(const DataLocation& loc) {
    static std::mutex m;
    static juce::String status;
    static bool done = false;
    std::lock_guard<std::mutex> lk(m);
    if (done) return status;
    done = true;

    loc.schwungDir.getChildFile("patches").createDirectory();
    loc.dataRoot.getChildFile("UserLibrary/Samples").createDirectory();

    const juce::File bundled = findBundledData();
    if (!bundled.isDirectory()) {
        status = loc.schwungDir.getChildFile("modules/chain/dsp.so").existsAsFile()
            ? "Using installed modules in " + loc.schwungDir.getFullPathName()
            : "No modules found. Install them into " + loc.schwungDir.getFullPathName() + "/modules";
        return status;
    }
    const auto id = bundled.getChildFile("build-id.txt").loadFileAsString().trim();
    const auto stamp = loc.schwungDir.getChildFile(".bundle-build-id");
    if (id.isNotEmpty() && stamp.loadFileAsString().trim() == id) {
        status = "Modules up to date";
        return status;
    }
    const int copied = copyTree(bundled.getChildFile("schwung"), loc.schwungDir);
    stamp.replaceWithText(id);
    status = "Installed " + juce::String(copied) + " files into " + loc.schwungDir.getFullPathName();
    return status;
}

} // namespace schwung
