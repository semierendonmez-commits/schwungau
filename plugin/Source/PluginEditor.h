#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "BitmapFont.h"

namespace ui {

// ---- palette: Move's graphite body, OLED, and pad colours by component ----
namespace col {
    const juce::Colour body   { 0xff2a2c2e };
    const juce::Colour panel  { 0xff35383b };
    const juce::Colour raised { 0xff404448 };
    const juce::Colour line   { 0xff4b5055 };
    const juce::Colour text   { 0xffd9dde1 };
    const juce::Colour dim    { 0xff8d9399 };
    const juce::Colour oledOn { 0xffddebf7 };
    const juce::Colour oledOff{ 0xff000000 };
    const juce::Colour synth  { 0xff3fa9f5 };
    const juce::Colour fx     { 0xfff2c14e };
    const juce::Colour midi   { 0xff9b7bff };
    inline juce::Colour forPosition(const juce::String& pos) {
        return pos == "synth" ? synth : pos.startsWith("midi_fx") ? midi : fx;
    }
}

struct ParamMeta {
    juce::String key, name, shortName, type = "float", unit;
    double min = 0, max = 1, step = 0;
    juce::StringArray options;
    bool known = false;
};

class SchwungLookAndFeel : public juce::LookAndFeel_V4 {
public:
    SchwungLookAndFeel();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos,
                          float start, float end, juce::Slider&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Colour accent = col::synth;
};

// Move-sized display, drawn with Schwung's Tamzen fonts.
class OledView : public juce::Component {
public:
    OledView();
    struct Content {
        juce::String title, position, patch;
        juce::StringArray chainAbbrevs; juce::Array<int> chainKinds; int focused = -1;
        juce::String paramName, paramValue; float paramNorm = -1.f;
    };
    void setContent(const Content& c) { content_ = c; redraw(); }
    void setParamLine(const juce::String& name, const juce::String& value, float norm);
    void paint(juce::Graphics&) override;
private:
    void redraw();
    BitmapFont small_, large_;
    Oled oled_;
    Content content_;
};

class ChainStrip : public juce::Component {
public:
    struct Slot { juce::String position, moduleId, label; bool bypassed = false; };
    std::function<void(const juce::String& position)> onSelect;
    std::function<void(const juce::String& position, bool add)> onPick;     // open browser
    std::function<void(const juce::String& position)> onRemove;
    std::function<void(const juce::String& position, bool bypass)> onBypass;

    void setSlots(const juce::Array<Slot>& midi, const Slot& synth, const juce::Array<Slot>& fx,
                  const juce::String& focused);
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
private:
    struct Box { juce::Rectangle<int> r; Slot slot; bool isAdd = false; };
    juce::Array<Box> boxes_;
    juce::Array<Slot> midi_, fx_;
    Slot synth_;
    juce::String focused_;
};

class ModuleBrowser : public juce::Component, private juce::ListBoxModel, private juce::TextEditor::Listener {
public:
    ModuleBrowser();
    std::function<void(const juce::String& moduleId)> onChoose;
    std::function<void()> onCancel;
    void show(const std::vector<schwung::ModuleInfo>& mods, const juce::String& position, const juce::String& current);
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
private:
    int getNumRows() override { return rows_.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void textEditorTextChanged(juce::TextEditor&) override { filter(); }
    void filter();
    struct Row { bool header = false; juce::String text, sub, id; };
    std::vector<schwung::ModuleInfo> all_;
    juce::Array<Row> rows_;
    juce::String position_, current_;
    juce::TextEditor search_;
    juce::ListBox list_;
    juce::TextButton none_ { "Remove module" }, cancel_ { "Close" };
};

// Parameter page for one chain position, generated from the module's own
// ui_hierarchy + chain_params, the same inputs the Shadow UI renders from.
class ParamPanel : public juce::Component, private juce::Timer {
public:
    explicit ParamPanel(SchwungProcessor& p);
    ~ParamPanel() override;
    void setPosition(const juce::String& position);     // "" = nothing focused
    void reload();                                       // module changed
    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    std::function<void(const juce::String& name, const juce::String& value, float norm)> onTouched;
private:
    void timerCallback() override;
    void buildLevel(const juce::String& level);
    juce::Component* makeControl(const ParamMeta& m, bool asKnob);
    void pushValue(const ParamMeta& m, double v);
    juce::String fmt(const ParamMeta& m, double v) const;
    ParamMeta metaFor(const juce::var& item) const;
    void refreshPresetName();

    SchwungProcessor& proc_;
    juce::String position_, level_ = "root";
    juce::StringArray levelStack_;
    juce::var hierarchy_;
    std::map<juce::String, ParamMeta> meta_;

    // header: breadcrumbs + preset browser
    juce::TextButton back_ { "Back" };
    juce::Label levelName_;
    juce::TextButton prevPreset_ { "<" }, nextPreset_ { ">" }, childLevel_ { "Edit" };
    juce::Label presetName_;
    juce::String listParam_, countParam_, nameParam_, childName_;

    struct Ctl { ParamMeta meta; std::unique_ptr<juce::Component> comp; std::unique_ptr<juce::Label> label; bool knob = false; };
    std::vector<Ctl> knobs_, rows_;
    std::vector<std::unique_ptr<juce::TextButton>> navs_;
    juce::Viewport viewport_;
    juce::Component rowsHolder_;
    juce::Label empty_;
    bool updating_ = false;
};

} // namespace ui

class SchwungEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit SchwungEditor(SchwungProcessor&);
    ~SchwungEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    void refreshChain();
    void focus(const juce::String& position);
    void openBrowser(const juce::String& position, bool add);
    void refreshPatches();

    SchwungProcessor& proc_;
    ui::SchwungLookAndFeel lnf_;
    ui::OledView oled_;
    ui::ChainStrip chain_;
    ui::ParamPanel params_;
    ui::ModuleBrowser browser_;
    juce::ComboBox recv_, fwd_, patches_;
    juce::Slider transpose_;
    juce::Label recvL_, fwdL_, trL_, status_;
    juce::TextButton savePatch_ { "Save patch" }, folder_ { "Modules folder" }, rescan_ { "Rescan" };
    juce::String focused_ = "synth", lastParamName_, lastParamValue_;
    float lastParamNorm_ = -1.f;
    bool paramDirty_ = false;
    int seenRevision_ = -1;
    int midiCount_ = 0, fxCount_ = 0;
};
