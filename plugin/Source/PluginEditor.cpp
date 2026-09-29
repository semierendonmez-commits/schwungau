#include "PluginEditor.h"
#include "BinaryData.h"

namespace ui {

// ============================================================================
// Look and feel
// ============================================================================
SchwungLookAndFeel::SchwungLookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, col::body);
    setColour(juce::Label::textColourId, col::text);
    setColour(juce::TextButton::buttonColourId, col::raised);
    setColour(juce::TextButton::textColourOffId, col::text);
    setColour(juce::TextButton::textColourOnId, col::text);
    setColour(juce::ComboBox::backgroundColourId, col::raised);
    setColour(juce::ComboBox::outlineColourId, col::line);
    setColour(juce::ComboBox::textColourId, col::text);
    setColour(juce::ComboBox::arrowColourId, col::dim);
    setColour(juce::PopupMenu::backgroundColourId, col::panel);
    setColour(juce::PopupMenu::textColourId, col::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, col::raised);
    setColour(juce::TextEditor::backgroundColourId, col::raised);
    setColour(juce::TextEditor::outlineColourId, col::line);
    setColour(juce::TextEditor::textColourId, col::text);
    setColour(juce::ListBox::backgroundColourId, col::panel);
    setColour(juce::Slider::textBoxTextColourId, col::text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::trackColourId, col::synth);
    setColour(juce::Slider::backgroundColourId, col::raised);
    setColour(juce::Slider::thumbColourId, col::text);
    setColour(juce::ScrollBar::thumbColourId, col::line);
}

// Move's knobs are plain grey caps; the value lives on the screen. Draw the
// cap and let a thin arc in the component colour carry the value.
void SchwungLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float start, float end, juce::Slider& s) {
    const auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(4.f);
    const float d = juce::jmin(r.getWidth(), r.getHeight());
    const auto c = r.withSizeKeepingCentre(d, d);
    const float ang = start + pos * (end - start);
    juce::Path track, val;
    track.addCentredArc(c.getCentreX(), c.getCentreY(), d / 2, d / 2, 0, start, end, true);
    val.addCentredArc(c.getCentreX(), c.getCentreY(), d / 2, d / 2, 0, start, ang, true);
    g.setColour(col::line);
    g.strokePath(track, juce::PathStrokeType(2.f));
    g.setColour(s.findColour(juce::Slider::trackColourId));
    g.strokePath(val, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const auto cap = c.reduced(d * 0.16f);
    g.setColour(juce::Colour(0xff9ea3a8));
    g.fillEllipse(cap);
    g.setColour(juce::Colour(0xffb9bdc1));
    g.fillEllipse(cap.reduced(cap.getWidth() * 0.12f));
    g.setColour(col::body);
    const auto p1 = c.getCentre().getPointOnCircumference(cap.getWidth() * 0.12f, ang);
    const auto p2 = c.getCentre().getPointOnCircumference(cap.getWidth() * 0.42f, ang);
    g.drawLine({ p1, p2 }, 2.f);
}

void SchwungLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                                              bool over, bool down) {
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    auto c = bg;
    if (down) c = c.brighter(0.15f); else if (over) c = c.brighter(0.07f);
    g.setColour(c);
    g.fillRoundedRectangle(r, 3.f);
}

// ============================================================================
// OLED
// ============================================================================
OledView::OledView() {
    small_.load(BinaryData::Tamzen6x12r_bdf, (size_t)BinaryData::Tamzen6x12r_bdfSize);
    large_.load(BinaryData::Tamzen8x16r_bdf, (size_t)BinaryData::Tamzen8x16r_bdfSize);
    setOpaque(true);
}

void OledView::redraw() {
    oled_.clear();
    auto& c = content_;
    // Title row: focused component, its position on the right.
    small_.draw(oled_.px, Oled::W, Oled::H, 1, 0, c.title.substring(0, 16));
    const int pw = small_.textWidth(c.position);
    small_.draw(oled_.px, Oled::W, Oled::H, Oled::W - pw - 1, 0, c.position);
    oled_.hline(0, 12, Oled::W);

    // Chain diagram: one box per position, the focused one inverted.
    const int n = c.chainAbbrevs.size();
    if (n > 0) {
        const int gap = 3;
        const int bw = juce::jmin(40, (Oled::W - gap * (n - 1)) / n);
        int x = (Oled::W - (bw * n + gap * (n - 1))) / 2;
        for (int i = 0; i < n; ++i) {
            const bool f = i == c.focused;
            oled_.rect(x, 16, bw, 14, f);
            auto label = c.chainAbbrevs[i];
            while (label.isNotEmpty() && small_.textWidth(label) > bw - 3) label = label.dropLastCharacters(1);
            small_.draw(oled_.px, Oled::W, Oled::H, x + (bw - small_.textWidth(label)) / 2, 17, label, !f);
            if (i < n - 1) oled_.set(x + bw + 1, 23);
            x += bw + gap;
        }
    }
    // Last touched parameter, Move-style: big value, name, bar.
    if (c.paramName.isNotEmpty()) {
        small_.draw(oled_.px, Oled::W, Oled::H, 1, 34, c.paramName.substring(0, 20));
        large_.draw(oled_.px, Oled::W, Oled::H, 1, 46, c.paramValue.substring(0, 15));
        if (c.paramNorm >= 0.f) {
            oled_.rect(80, 50, 46, 8, false);
            oled_.rect(82, 52, (int)std::round(42 * juce::jlimit(0.f, 1.f, c.paramNorm)), 4, true);
        }
    } else {
        small_.draw(oled_.px, Oled::W, Oled::H, 1, 36, c.patch.isNotEmpty() ? c.patch : juce::String("Turn a knob"));
    }
    repaint();
}

void OledView::setParamLine(const juce::String& n, const juce::String& v, float norm) {
    content_.paramName = n; content_.paramValue = v; content_.paramNorm = norm;
    redraw();
}

void OledView::paint(juce::Graphics& g) {
    g.fillAll(col::body);
    auto r = getLocalBounds().toFloat();
    // Keep Move's 2:1 aspect, centred, inside a bezel.
    const float scale = juce::jmin((r.getWidth() - 16) / Oled::W, (r.getHeight() - 16) / Oled::H);
    auto screen = juce::Rectangle<float>(Oled::W * scale, Oled::H * scale).withCentre(r.getCentre());
    g.setColour(juce::Colours::black);
    g.fillRoundedRectangle(screen.expanded(6), 6.f);
    oled_.paint(g, screen, col::oledOn, col::oledOff);
}

// ============================================================================
// Chain strip
// ============================================================================
void ChainStrip::setSlots(const juce::Array<Slot>& midi, const Slot& synth, const juce::Array<Slot>& fx,
                          const juce::String& focused) {
    midi_ = midi; synth_ = synth; fx_ = fx; focused_ = focused;
    resized();
    repaint();
}

void ChainStrip::resized() {
    boxes_.clear();
    auto r = getLocalBounds().reduced(8, 6);
    const int h = r.getHeight();
    const int small = 92, big = 150, add = 30, gap = 6, arrow = 16;
    int x = r.getX();
    auto push = [&](int w, const Slot& s, bool isAdd) {
        boxes_.add({ juce::Rectangle<int>(x, r.getY(), w, h), s, isAdd });
        x += w + gap;
    };
    for (auto& s : midi_) push(small, s, false);
    push(add, Slot { "midi_fx" + juce::String(midi_.size() + 1), {}, "+" }, true);
    x += arrow;
    push(big, synth_, false);
    x += arrow;
    for (auto& s : fx_) push(small, s, false);
    push(add, Slot { "fx" + juce::String(fx_.size() + 1), {}, "+" }, true);
}

void ChainStrip::paint(juce::Graphics& g) {
    g.fillAll(col::panel);
    for (auto& b : boxes_) {
        const auto colr = col::forPosition(b.slot.position);
        auto r = b.r.toFloat();
        if (b.isAdd) {
            g.setColour(colr.withAlpha(0.55f));
            g.drawRoundedRectangle(r.reduced(0.5f), 4.f, 1.f);
            g.drawText("+", b.r, juce::Justification::centred);
            continue;
        }
        const bool empty = b.slot.moduleId.isEmpty();
        const bool f = b.slot.position == focused_;
        // A pad: lit in its component colour when loaded, dark when empty.
        g.setColour(empty ? col::raised : colr.withAlpha(b.slot.bypassed ? 0.25f : 0.85f));
        g.fillRoundedRectangle(r, 4.f);
        if (f) { g.setColour(col::text); g.drawRoundedRectangle(r.reduced(1.f), 4.f, 2.f); }
        g.setColour(empty ? col::dim : juce::Colours::black.withAlpha(0.85f));
        g.setFont(juce::FontOptions(14.f, juce::Font::bold));
        g.drawFittedText(empty ? juce::String("Choose synth") : b.slot.label, b.r.reduced(6, 2),
                         juce::Justification::centred, 2);
    }
    // Signal-flow joins between the three groups.
    g.setColour(col::dim);
    for (int i = 0; i + 1 < boxes_.size(); ++i) {
        const bool groupChange = boxes_[i].isAdd || boxes_[i].slot.position == "synth";
        if (!groupChange) continue;
        const float y = (float)boxes_[i].r.getCentreY();
        const float x0 = (float)boxes_[i].r.getRight() + 3, x1 = (float)boxes_[i + 1].r.getX() - 3;
        if (x1 - x0 < 6) continue;
        juce::Path p; p.addArrow({ x0, y, x1, y }, 1.2f, 7.f, 6.f);
        g.fillPath(p);
    }
}

void ChainStrip::mouseUp(const juce::MouseEvent& e) {
    for (auto& b : boxes_) {
        if (!b.r.contains(e.getPosition())) continue;
        if (b.isAdd) { if (onPick) onPick(b.slot.position, true); return; }
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu m;
            m.addItem(1, "Change module...");
            if (b.slot.moduleId.isNotEmpty()) {
                m.addItem(2, "Bypass", true, b.slot.bypassed);
                m.addItem(3, b.slot.position == "synth" ? "Unload synth" : "Remove");
            }
            const auto pos = b.slot.position; const bool byp = b.slot.bypassed;
            m.showMenuAsync(juce::PopupMenu::Options(), [this, pos, byp](int r) {
                if (r == 1 && onPick) onPick(pos, false);
                if (r == 2 && onBypass) onBypass(pos, !byp);
                if (r == 3 && onRemove) onRemove(pos);
            });
            return;
        }
        if (b.slot.moduleId.isEmpty()) { if (onPick) onPick(b.slot.position, false); return; }
        if (onSelect) onSelect(b.slot.position);
        return;
    }
}

// ============================================================================
// Module browser
// ============================================================================
ModuleBrowser::ModuleBrowser() {
    search_.setTextToShowWhenEmpty("Search by name, type or author", col::dim);
    search_.addListener(this);
    addAndMakeVisible(search_);
    list_.setModel(this);
    list_.setRowHeight(26);
    addAndMakeVisible(list_);
    addAndMakeVisible(none_);
    addAndMakeVisible(cancel_);
    none_.onClick = [this] { if (onChoose) onChoose({}); };
    cancel_.onClick = [this] { if (onCancel) onCancel(); };
    setWantsKeyboardFocus(true);
    setOpaque(true);
}

void ModuleBrowser::show(const std::vector<schwung::ModuleInfo>& mods, const juce::String& position,
                         const juce::String& current) {
    all_ = mods; position_ = position; current_ = current;
    search_.clear();
    none_.setVisible(current.isNotEmpty());
    filter();
    setVisible(true);
    search_.grabKeyboardFocus();
}

void ModuleBrowser::filter() {
    rows_.clear();
    const auto q = search_.getText().trim().toLowerCase();
    std::map<juce::String, juce::Array<Row>> groups;
    for (auto& m : all_) {
        if (!m.chainable) continue;
        const juce::String hay = juce::String(m.name + " " + m.id + " " + m.subcategory + " " + m.author + " " + m.description).toLowerCase();
        if (q.isNotEmpty() && !hay.contains(q)) continue;
        juce::String sub = juce::String(m.subcategory).replaceCharacter('-', ' ');
        if (sub.isEmpty()) sub = "other";
        groups[sub].add({ false, juce::String(m.name), juce::String(m.author), juce::String(m.id) });
    }
    for (auto& [sub, rs] : groups) {
        rows_.add({ true, sub.substring(0, 1).toUpperCase() + sub.substring(1), {}, {} });
        rows_.addArray(rs);
    }
    list_.updateContent();
    list_.repaint();
    for (int i = 0; i < rows_.size(); ++i) if (!rows_[i].header) { list_.selectRow(i); break; }
}

void ModuleBrowser::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= rows_.size()) return;
    const auto& r = rows_.getReference(row);
    if (r.header) {
        g.setColour(col::dim);
        g.setFont(juce::FontOptions(12.f, juce::Font::bold));
        g.drawText(r.text, 10, 0, w - 20, h, juce::Justification::bottomLeft);
        return;
    }
    if (selected) { g.setColour(col::raised); g.fillRect(0, 0, w, h); }
    const auto accent = col::forPosition(position_);
    if (r.id == current_) { g.setColour(accent); g.fillRect(4, 6, 3, h - 12); }
    g.setColour(col::text);
    g.setFont(juce::FontOptions(15.f));
    g.drawText(r.text, 14, 0, w / 2, h, juce::Justification::centredLeft);
    g.setColour(col::dim);
    g.setFont(juce::FontOptions(12.f));
    g.drawText(r.sub, w / 2, 0, w / 2 - 12, h, juce::Justification::centredRight);
}

void ModuleBrowser::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { returnKeyPressed(row); }

void ModuleBrowser::returnKeyPressed(int row) {
    if (row >= 0 && row < rows_.size() && !rows_[row].header && onChoose) onChoose(rows_[row].id);
}

bool ModuleBrowser::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::escapeKey) { if (onCancel) onCancel(); return true; }
    if (k == juce::KeyPress::returnKey) { returnKeyPressed(list_.getSelectedRow()); return true; }
    if (k == juce::KeyPress::downKey || k == juce::KeyPress::upKey) {
        int r = list_.getSelectedRow();
        const int d = k == juce::KeyPress::downKey ? 1 : -1;
        for (int i = r + d; i >= 0 && i < rows_.size(); i += d)
            if (!rows_[i].header) { list_.selectRow(i); break; }
        return true;
    }
    return false;
}

void ModuleBrowser::paint(juce::Graphics& g) {
    g.fillAll(col::panel);
    g.setColour(col::forPosition(position_));
    g.fillRect(0, 0, getWidth(), 3);
    g.setColour(col::text);
    g.setFont(juce::FontOptions(16.f, juce::Font::bold));
    const juce::String what = position_ == "synth" ? "Choose a sound generator"
                            : position_.startsWith("midi_fx") ? "Choose a MIDI effect" : "Choose an audio effect";
    g.drawText(what, 12, 10, getWidth() - 24, 22, juce::Justification::centredLeft);
}

void ModuleBrowser::resized() {
    auto r = getLocalBounds().reduced(12);
    r.removeFromTop(28);
    search_.setBounds(r.removeFromTop(28));
    r.removeFromTop(8);
    auto bottom = r.removeFromBottom(30);
    cancel_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    none_.setBounds(bottom.removeFromRight(130));
    r.removeFromBottom(8);
    list_.setBounds(r);
}

// ============================================================================
// Parameter panel
// ============================================================================
ParamPanel::ParamPanel(SchwungProcessor& p) : proc_(p) {
    for (auto* c : { (juce::Component*)&back_, (juce::Component*)&levelName_, (juce::Component*)&prevPreset_,
                     (juce::Component*)&nextPreset_, (juce::Component*)&presetName_, (juce::Component*)&childLevel_,
                     (juce::Component*)&viewport_, (juce::Component*)&empty_ })
        addChildComponent(c);
    levelName_.setFont(juce::FontOptions(15.f, juce::Font::bold));
    presetName_.setJustificationType(juce::Justification::centred);
    presetName_.setColour(juce::Label::backgroundColourId, col::raised);
    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, col::dim);
    viewport_.setViewedComponent(&rowsHolder_, false);
    viewport_.setScrollBarsShown(true, false);
    back_.onClick = [this] {
        if (levelStack_.isEmpty()) return;
        const auto l = levelStack_[levelStack_.size() - 1];
        levelStack_.remove(levelStack_.size() - 1);
        buildLevel(l);
    };
    auto step = [this](int d) {
        const auto key = position_ + ":" + listParam_;
        const int count = juce::String(proc_.engine().getParam((position_ + ":" + countParam_).toStdString())).getIntValue();
        int cur = juce::String(proc_.engine().getParam(key.toStdString())).getIntValue();
        if (count > 0) cur = (cur + d + count) % count; else cur = juce::jmax(0, cur + d);
        proc_.engine().setParam(key.toStdString(), std::to_string(cur));
        refreshPresetName();
    };
    prevPreset_.onClick = [step] { step(-1); };
    nextPreset_.onClick = [step] { step(1); };
    childLevel_.onClick = [this] { levelStack_.add(level_); buildLevel(childName_); };
    startTimerHz(8);
}

ParamPanel::~ParamPanel() { stopTimer(); }

void ParamPanel::setPosition(const juce::String& position) {
    if (position == position_) return;
    position_ = position;
    reload();
}

ParamMeta ParamPanel::metaFor(const juce::var& item) const {
    ParamMeta m;
    const juce::String key = item.isString() ? item.toString() : item["key"].toString();
    auto it = meta_.find(key);
    if (it != meta_.end()) m = it->second;
    m.key = key;
    if (item.isObject()) {
        // Inline metadata in the hierarchy refines chain_params.
        if (item.hasProperty("label")) m.name = item["label"].toString();
        if (item.hasProperty("name") && m.name.isEmpty()) m.name = item["name"].toString();
        if (item.hasProperty("short_name")) m.shortName = item["short_name"].toString();
        if (item.hasProperty("type")) { m.type = item["type"].toString(); m.known = true; }
        if (item.hasProperty("min")) m.min = (double)item["min"];
        if (item.hasProperty("max")) m.max = (double)item["max"];
        if (item.hasProperty("step")) m.step = (double)item["step"];
        if (item.hasProperty("unit")) m.unit = item["unit"].toString();
        if (auto* o = item["options"].getArray()) { m.options.clear(); for (auto& v : *o) m.options.add(v.toString()); }
    }
    if (m.name.isEmpty()) m.name = key;
    return m;
}

void ParamPanel::reload() {
    meta_.clear();
    hierarchy_ = juce::var();
    levelStack_.clear();
    if (position_.isEmpty()) { buildLevel({}); return; }
    const auto prefix = position_.toStdString();
    const juce::String hj = proc_.engine().getParam(prefix + ":ui_hierarchy", 1 << 18);
    const juce::String cj = proc_.engine().getParam(prefix + ":chain_params", 1 << 18);
    hierarchy_ = juce::JSON::parse(hj);
    const juce::var params = juce::JSON::parse(cj);     // must outlive the loop below
    if (auto* arr = params.getArray()) {
        for (auto& v : *arr) {
            ParamMeta m;
            m.key = v["key"].toString();
            m.name = v["name"].toString();
            m.shortName = v["short_name"].toString();
            m.type = v.hasProperty("type") ? v["type"].toString() : "float";
            m.min = v.hasProperty("min") ? (double)v["min"] : 0.0;
            m.max = v.hasProperty("max") ? (double)v["max"] : 1.0;
            m.step = v.hasProperty("step") ? (double)v["step"] : 0.0;
            m.unit = v["unit"].toString();
            if (auto* o = v["options"].getArray()) for (auto& x : *o) m.options.add(x.toString());
            m.known = true;
            if (m.key.isNotEmpty()) meta_[m.key] = m;
        }
    }
    buildLevel("root");
}

juce::String ParamPanel::fmt(const ParamMeta& m, double v) const {
    if (m.type == "enum") {
        const int i = (int)std::lround(v);
        return juce::isPositiveAndBelow(i, m.options.size()) ? m.options[i] : juce::String(i);
    }
    if (m.type == "int") return juce::String((int)std::lround(v)) + (m.unit.isNotEmpty() && m.unit != "%" ? " " + m.unit : "");
    if (m.unit == "%" && m.max <= 1.0) return juce::String(v * 100.0, 0) + "%";
    return juce::String(v, std::abs(m.max - m.min) >= 100 ? 0 : 2) + (m.unit.isNotEmpty() ? " " + m.unit : "");
}

void ParamPanel::pushValue(const ParamMeta& m, double v) {
    if (updating_) return;
    juce::String s;
    if (m.type == "enum") {
        // Latched per key from what get_param reports (param_format.mjs).
        const juce::String cur = proc_.engine().getParam((position_ + ":" + m.key).toStdString(), 256);
        const int i = (int)std::lround(v);
        const bool names = m.options.contains(cur) && !cur.containsOnly("-0123456789.");
        s = names && juce::isPositiveAndBelow(i, m.options.size()) ? m.options[i] : juce::String(i);
    } else if (m.type == "int") s = juce::String((int)std::lround(v));
    else s = juce::String(v, 4);
    proc_.engine().setParam((position_ + ":" + m.key).toStdString(), s.toStdString());
    const double norm = m.max > m.min ? (v - m.min) / (m.max - m.min) : -1.0;
    if (onTouched) onTouched(m.name, fmt(m, v), (float)norm);
}

juce::Component* ParamPanel::makeControl(const ParamMeta& m, bool asKnob) {
    const auto accent = col::forPosition(position_);
    if (m.type == "enum" && !asKnob) {
        auto* cb = new juce::ComboBox();
        for (int i = 0; i < m.options.size(); ++i) cb->addItem(m.options[i], i + 1);
        cb->onChange = [this, cb, m] { pushValue(m, cb->getSelectedId() - 1); };
        return cb;
    }
    auto* s = new juce::Slider(asKnob ? juce::Slider::RotaryHorizontalVerticalDrag : juce::Slider::LinearBar,
                               juce::Slider::NoTextBox);
    double lo = m.min, hi = m.max, st = m.step;
    if (m.type == "enum") { lo = 0; hi = juce::jmax(0, m.options.size() - 1); st = 1; }
    if (m.type == "int") st = 1;
    if (hi <= lo) hi = lo + 1;
    s->setRange(lo, hi, st > 0 ? st : 0);
    s->setColour(juce::Slider::trackColourId, accent);
    s->setColour(juce::Slider::backgroundColourId, col::raised);
    s->textFromValueFunction = [this, m](double v) { return fmt(m, v); };
    s->setPopupDisplayEnabled(asKnob, true, this);
    s->onValueChange = [this, s, m] { pushValue(m, s->getValue()); };
    if (asKnob) {
        // Right-click: bind this parameter to one of the eight DAW knobs
        // through the chain's own knob mappings (knob_N_set = "pos:key").
        s->addMouseListener(this, false);
        s->getProperties().set("paramKey", m.key);
    }
    return s;
}

void ParamPanel::mouseUp(const juce::MouseEvent& e) {
    if (!e.mods.isPopupMenu() || e.eventComponent == nullptr) return;
    const juce::String key = e.eventComponent->getProperties()["paramKey"].toString();
    if (key.isEmpty()) return;
    // Which DAW knob already drives this parameter?
    int bound = 0;
    for (int i = 1; i <= 8; ++i) {
        const auto n = std::to_string(i);
        if (juce::String(proc_.engine().getParam("knob_" + n + "_target", 64)) == position_ &&
            juce::String(proc_.engine().getParam("knob_" + n + "_param", 96)) == key) bound = i;
    }
    juce::PopupMenu m;
    m.addSectionHeader("Automate with DAW knob");
    for (int i = 1; i <= 8; ++i) {
        const juce::String cur = proc_.engine().getParam("knob_" + std::to_string(i) + "_name", 128);
        m.addItem(i, "Knob " + juce::String(i) + (cur.isNotEmpty() ? "   (" + cur + ")" : juce::String()), true, bound == i);
    }
    if (bound) m.addItem(100, "Clear knob " + juce::String(bound));
    const auto pos = position_;
    m.showMenuAsync(juce::PopupMenu::Options(), [this, key, pos, bound](int r) {
        if (r >= 1 && r <= 8)
            proc_.engine().setParam("knob_" + std::to_string(r) + "_set", (pos + ":" + key).toStdString());
        else if (r == 100)
            proc_.engine().setParam("knob_" + std::to_string(bound) + "_clear", "1");
    });
}

void ParamPanel::buildLevel(const juce::String& level) {
    knobs_.clear(); rows_.clear(); navs_.clear();
    rowsHolder_.removeAllChildren();
    level_ = level;
    listParam_ = countParam_ = nameParam_ = childName_ = {};
    const bool none = position_.isEmpty();
    empty_.setVisible(none);
    empty_.setText(none ? "Load a module to see its parameters" : "", juce::dontSendNotification);
    for (auto* c : { (juce::Component*)&back_, (juce::Component*)&levelName_, (juce::Component*)&viewport_ })
        c->setVisible(!none);
    if (none) { resized(); return; }

    juce::var lv = hierarchy_["levels"][juce::Identifier(level.isEmpty() ? "root" : level)];
    juce::var knobKeys, params;
    if (lv.isObject()) {
        knobKeys = lv["knobs"]; params = lv["params"];
        listParam_ = lv["list_param"].toString();
        countParam_ = lv["count_param"].toString();
        nameParam_ = lv["name_param"].toString();
        if (lv["children"].isString()) childName_ = lv["children"].toString();
        juce::String nm = lv["name"].toString();
        if (nm.isEmpty()) nm = lv["label"].toString();
        levelName_.setText(nm.isNotEmpty() ? nm : (level == "root" ? juce::String("Main") : level), juce::dontSendNotification);
    } else {
        // No hierarchy: fall back to chain_params, knobs first.
        juce::Array<juce::var> ks, ps;
        for (auto& [k, m] : meta_) { (ks.size() < 8 ? ks : ps).add(k); }
        knobKeys = ks; params = ps;
        levelName_.setText("Parameters", juce::dontSendNotification);
    }
    back_.setVisible(!levelStack_.isEmpty());
    const bool preset = listParam_.isNotEmpty();
    for (auto* c : { (juce::Component*)&prevPreset_, (juce::Component*)&nextPreset_, (juce::Component*)&presetName_ })
        c->setVisible(preset);
    childLevel_.setVisible(childName_.isNotEmpty());
    if (preset) refreshPresetName();


    juce::StringArray onKnobs;
    if (auto* ka = knobKeys.getArray())
        for (auto& k : *ka) {
            if (knobs_.size() >= 8) break;
            const auto m = metaFor(k);
            if (m.key.isEmpty()) continue;
            Ctl c; c.meta = m; c.knob = true;
            c.comp.reset(makeControl(m, true));
            c.label = std::make_unique<juce::Label>();
            c.label->setText(m.shortName.isNotEmpty() ? m.shortName : m.name, juce::dontSendNotification);
            c.label->setJustificationType(juce::Justification::centred);
            c.label->setFont(juce::FontOptions(12.f));
            addAndMakeVisible(*c.comp); addAndMakeVisible(*c.label);
            onKnobs.add(m.key);
            knobs_.push_back(std::move(c));
        }
    // Move enters a preset level's "children" level with a jog click. On a
    // desktop there is room for both: when this level has no rows of its own,
    // show the child level's rows and page links underneath.
    juce::var shown = params;
    if (childName_.isNotEmpty() && !(params.getArray() && params.getArray()->size() > 0)) {
        juce::var child = hierarchy_["levels"][juce::Identifier(childName_)];
        if (child.isObject()) { shown = child["params"]; childLevel_.setVisible(false); }
    }
    if (auto* pa = shown.getArray())
        for (auto& it : *pa) {
            if (it.isObject() && it.hasProperty("level")) {
                auto b = std::make_unique<juce::TextButton>(it["label"].toString().isNotEmpty() ? it["label"].toString()
                                                                                                 : it["level"].toString());
                const auto target = it["level"].toString();
                b->onClick = [this, target] { levelStack_.add(level_); buildLevel(target); };
                rowsHolder_.addAndMakeVisible(*b);
                navs_.push_back(std::move(b));
                continue;
            }
            const auto m = metaFor(it);
            if (m.key.isEmpty() || onKnobs.contains(m.key) || m.key == listParam_) continue;
            if (m.type != "float" && m.type != "int" && m.type != "enum") continue;   // file pickers etc.
            Ctl c; c.meta = m;
            c.comp.reset(makeControl(m, false));
            c.label = std::make_unique<juce::Label>();
            c.label->setText(m.name, juce::dontSendNotification);
            c.label->setFont(juce::FontOptions(13.f));
            rowsHolder_.addAndMakeVisible(*c.comp); rowsHolder_.addAndMakeVisible(*c.label);
            rows_.push_back(std::move(c));
        }
    resized();
    timerCallback();
}

void ParamPanel::refreshPresetName() {
    if (listParam_.isEmpty()) return;
    auto& e = proc_.engine();
    juce::String name = nameParam_.isNotEmpty() ? juce::String(e.getParam((position_ + ":" + nameParam_).toStdString(), 256)) : juce::String();
    const auto idx = juce::String(e.getParam((position_ + ":" + listParam_).toStdString(), 64));
    const auto cnt = juce::String(e.getParam((position_ + ":" + countParam_).toStdString(), 64));
    if (name.isEmpty()) name = "Preset " + idx;
    presetName_.setText(name + (cnt.isNotEmpty() ? "   " + juce::String(idx.getIntValue() + 1) + "/" + cnt : juce::String()),
                        juce::dontSendNotification);
}

void ParamPanel::timerCallback() {
    if (position_.isEmpty() || !isShowing()) return;
    updating_ = true;
    auto upd = [this](Ctl& c) {
        const juce::String v = proc_.engine().getParam((position_ + ":" + c.meta.key).toStdString(), 256);
        if (v.isEmpty()) return;
        double d = v.getDoubleValue();
        if (c.meta.type == "enum") { const int i = c.meta.options.indexOf(v); if (i >= 0) d = i; }
        if (auto* s = dynamic_cast<juce::Slider*>(c.comp.get())) {
            if (!s->isMouseButtonDown()) s->setValue(d, juce::dontSendNotification);
        } else if (auto* cb = dynamic_cast<juce::ComboBox*>(c.comp.get())) {
            cb->setSelectedId((int)std::lround(d) + 1, juce::dontSendNotification);
        }
    };
    for (auto& c : knobs_) upd(c);
    for (auto& c : rows_) upd(c);
    updating_ = false;
    if (listParam_.isNotEmpty()) refreshPresetName();
}

void ParamPanel::paint(juce::Graphics& g) {
    g.fillAll(col::panel);
    if (!knobs_.empty()) {
        g.setColour(col::line);
        g.drawHorizontalLine(knobs_.front().label->getBottom() + 8, 10.f, (float)getWidth() - 10.f);
    }
}

void ParamPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    empty_.setBounds(r);
    auto head = r.removeFromTop(28);
    if (back_.isVisible()) { back_.setBounds(head.removeFromLeft(60)); head.removeFromLeft(8); }
    if (childLevel_.isVisible()) { childLevel_.setBounds(head.removeFromRight(60)); head.removeFromRight(8); }
    if (presetName_.isVisible()) {
        auto p = head.removeFromRight(juce::jmin(320, head.getWidth() / 2));
        prevPreset_.setBounds(p.removeFromLeft(28));
        nextPreset_.setBounds(p.removeFromRight(28));
        presetName_.setBounds(p.reduced(4, 0));
    }
    levelName_.setBounds(head);
    r.removeFromTop(8);
    if (!knobs_.empty()) {
        auto kr = r.removeFromTop(96);
        const int w = kr.getWidth() / 8;
        for (size_t i = 0; i < knobs_.size(); ++i) {
            auto cell = kr.withX(kr.getX() + (int)i * w).withWidth(w);
            knobs_[i].label->setBounds(cell.removeFromBottom(18));
            knobs_[i].comp->setBounds(cell.reduced(6, 2));
        }
        r.removeFromTop(16);
    }
    viewport_.setBounds(r);
    const int rowH = 26, width = r.getWidth() - 12;
    int y = 0;
    const int colW = width / 2;
    int col = 0;
    for (auto& c : rows_) {
        const int x = col * colW;
        c.label->setBounds(x, y, colW * 2 / 5, rowH - 4);
        c.comp->setBounds(x + colW * 2 / 5, y + 2, colW * 3 / 5 - 12, rowH - 6);
        if (++col == 2) { col = 0; y += rowH; }
    }
    if (col) y += rowH;
    if (!navs_.empty()) y += 6;
    int nx = 0;
    for (auto& b : navs_) {
        const int bw = juce::jmax(90, b->getBestWidthForHeight(24) + 16);
        if (nx + bw > width) { nx = 0; y += 30; }
        b->setBounds(nx, y, bw, 24);
        nx += bw + 6;
    }
    if (!navs_.empty()) y += 30;
    rowsHolder_.setSize(width, juce::jmax(y, 1));
}

} // namespace ui

// ============================================================================
// Editor
// ============================================================================
SchwungEditor::SchwungEditor(SchwungProcessor& p)
    : AudioProcessorEditor(p), proc_(p), params_(p) {
    setLookAndFeel(&lnf_);
    addAndMakeVisible(oled_);
    addAndMakeVisible(chain_);
    addAndMakeVisible(params_);
    addChildComponent(browser_);

    chain_.onSelect = [this](const juce::String& pos) { focus(pos); };
    chain_.onPick = [this](const juce::String& pos, bool add) { openBrowser(pos, add); };
    chain_.onRemove = [this](const juce::String& pos) {
        if (pos == "synth") proc_.loadModule("synth", {});
        else if (pos.startsWith("midi_fx")) proc_.engine().setParam("midi_fx:remove", pos.fromLastOccurrenceOf("midi_fx", false, false).toStdString());
        else proc_.engine().setParam("fx:remove", pos.fromLastOccurrenceOf("fx", false, false).toStdString());
        ++proc_.chainRevision;
        if (pos == focused_) focused_ = "synth";
    };
    chain_.onBypass = [this](const juce::String& pos, bool b) {
        proc_.engine().setParam((pos + ":bypassed").toStdString(), b ? "1" : "0");
        ++proc_.chainRevision;
    };
    browser_.onCancel = [this] { browser_.setVisible(false); };
    params_.onTouched = [this](const juce::String& n, const juce::String& v, float norm) {
        lastParamName_ = n; lastParamValue_ = v; lastParamNorm_ = norm; paramDirty_ = true;
    };

    // Routing: the slot-level settings the shim owns on Move.
    recv_.addItem("All", 1);
    for (int c = 1; c <= 16; ++c) recv_.addItem(juce::String(c), c + 1);
    fwd_.addItem("Auto", 1); fwd_.addItem("Thru", 2);
    for (int c = 1; c <= 16; ++c) fwd_.addItem(juce::String(c), c + 2);
    transpose_.setSliderStyle(juce::Slider::IncDecButtons);
    transpose_.setRange(-48, 48, 1);
    transpose_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, 22);
    auto pushRouting = [this] {
        schwung::SlotRouting r;
        r.receiveChannel = recv_.getSelectedId() <= 1 ? -1 : recv_.getSelectedId() - 2;
        const int f = fwd_.getSelectedId();
        r.forwardChannel = f <= 1 ? -1 : f == 2 ? -2 : f - 3;
        r.transpose = (int)transpose_.getValue();
        proc_.engine().setRouting(r);
    };
    recv_.onChange = pushRouting; fwd_.onChange = pushRouting; transpose_.onValueChange = pushRouting;
    recvL_.setText("Receive", juce::dontSendNotification);
    fwdL_.setText("Forward", juce::dontSendNotification);
    trL_.setText("Transpose", juce::dontSendNotification);
    for (auto* l : { &recvL_, &fwdL_, &trL_ }) l->setColour(juce::Label::textColourId, ui::col::dim);
    status_.setColour(juce::Label::textColourId, ui::col::dim);
    status_.setJustificationType(juce::Justification::centredRight);
    patches_.setTextWhenNothingSelected("Patches");
    patches_.onChange = [this] {
        const int idx = patches_.getSelectedId() - 1;
        if (idx < 0) return;
        proc_.engine().setParam("load_patch", std::to_string(idx));
        ++proc_.chainRevision;
        proc_.setStatus("Loaded patch " + patches_.getText());
    };
    savePatch_.onClick = [this] {
        auto* w = new juce::AlertWindow("Save patch", "Saved to the Schwung patch library, shared with Move.",
                                        juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor("name", "My patch");
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
            if (r == 1) {
                juce::var doc = juce::JSON::parse(juce::String(proc_.engine().saveSlotState()));
                juce::var chain = doc["chain"];
                if (auto* o = chain.getDynamicObject()) {
                    o->setProperty("custom_name", w->getTextEditorContents("name"));
                    proc_.engine().setParam("save_patch", juce::JSON::toString(chain, true).toStdString());
                    proc_.setStatus("Saved patch " + w->getTextEditorContents("name"));
                    refreshPatches();
                }
            }
        }), true);
    };
    folder_.onClick = [this] { proc_.data().schwungDir.getChildFile("modules").revealToUser(); };
    rescan_.onClick = [this] { proc_.rescanModules(); proc_.setStatus(juce::String(proc_.catalog().all().size()) + " modules installed"); };
    for (auto* c : std::initializer_list<juce::Component*>{ &recv_, &fwd_, &transpose_, &recvL_, &fwdL_, &trL_,
                                                            &status_, &patches_, &savePatch_, &folder_, &rescan_ })
        addAndMakeVisible(c);

    const auto r = proc_.engine().routing();
    recv_.setSelectedId(r.receiveChannel < 0 ? 1 : r.receiveChannel + 2, juce::dontSendNotification);
    fwd_.setSelectedId(r.forwardChannel == -1 ? 1 : r.forwardChannel == -2 ? 2 : r.forwardChannel + 3, juce::dontSendNotification);
    transpose_.setValue(r.transpose, juce::dontSendNotification);

    setResizable(true, true);
    setResizeLimits(760, 520, 1600, 1100);
    setSize(900, 600);
    refreshPatches();
    refreshChain();
    startTimerHz(15);
}

SchwungEditor::~SchwungEditor() { stopTimer(); setLookAndFeel(nullptr); }

void SchwungEditor::paint(juce::Graphics& g) { g.fillAll(ui::col::body); }

void SchwungEditor::resized() {
    auto r = getLocalBounds();
    oled_.setBounds(r.removeFromTop(juce::jlimit(150, 240, getHeight() / 4 + 40)));
    chain_.setBounds(r.removeFromTop(64));
    auto bottom = r.removeFromBottom(40).reduced(10, 6);
    recvL_.setBounds(bottom.removeFromLeft(56)); recv_.setBounds(bottom.removeFromLeft(64)); bottom.removeFromLeft(10);
    fwdL_.setBounds(bottom.removeFromLeft(58)); fwd_.setBounds(bottom.removeFromLeft(70)); bottom.removeFromLeft(10);
    trL_.setBounds(bottom.removeFromLeft(66)); transpose_.setBounds(bottom.removeFromLeft(96)); bottom.removeFromLeft(16);
    patches_.setBounds(bottom.removeFromLeft(150)); bottom.removeFromLeft(6);
    savePatch_.setBounds(bottom.removeFromLeft(90)); bottom.removeFromLeft(6);
    rescan_.setBounds(bottom.removeFromRight(64)); bottom.removeFromRight(6);
    folder_.setBounds(bottom.removeFromRight(110)); bottom.removeFromRight(6);
    status_.setBounds(bottom);
    params_.setBounds(r);
    browser_.setBounds(getLocalBounds().withTrimmedTop(oled_.getBottom()).reduced(40, 10));
}

void SchwungEditor::refreshPatches() {
    patches_.clear(juce::dontSendNotification);
    const int n = juce::String(proc_.engine().getParam("patch_count", 32)).getIntValue();
    for (int i = 0; i < n; ++i)
        patches_.addItem(juce::String(proc_.engine().getParam("patch_name_" + std::to_string(i), 128)), i + 1);
}

void SchwungEditor::refreshChain() {
    auto& e = proc_.engine();
    auto label = [this](const std::string& id) -> juce::String {
        if (id.empty()) return {};
        if (auto* m = proc_.catalog().find(id)) return m->name.empty() ? juce::String(id) : juce::String(m->name);
        return juce::String(id);
    };
    juce::Array<ui::ChainStrip::Slot> mfx, fx;
    ui::ChainStrip::Slot synth;
    juce::StringArray abbrevs; juce::Array<int> kinds; int focusedIndex = -1;
    auto abbrev = [this](const std::string& id) -> juce::String {
        if (auto* m = proc_.catalog().find(id)) if (!m->abbrev.empty()) return juce::String(m->abbrev);
        return juce::String(id).toUpperCase().substring(0, 5);
    };
    e.call([&](schwung::Engine& en) {
        for (int i = 1; i <= 8; ++i) {
            const auto id = en.getParamUnlocked("midi_fx" + std::to_string(i) + "_module", 128);
            if (id.empty()) continue;
            const auto pos = "midi_fx" + juce::String(i);
            mfx.add({ pos, juce::String(id), label(id), en.getParamUnlocked((pos + ":bypassed").toStdString(), 8) == "1" });
        }
        const auto sid = en.getParamUnlocked("synth_module", 128);
        synth = { "synth", juce::String(sid), label(sid), en.getParamUnlocked("synth:bypassed", 8) == "1" };
        for (int i = 1; i <= 8; ++i) {
            const auto id = en.getParamUnlocked("fx" + std::to_string(i) + "_module", 128);
            if (id.empty()) continue;
            const auto pos = "fx" + juce::String(i);
            fx.add({ pos, juce::String(id), label(id), en.getParamUnlocked((pos + ":bypassed").toStdString(), 8) == "1" });
        }
    });
    midiCount_ = mfx.size(); fxCount_ = fx.size();
    // Focus falls back to the synth when its position vanished.
    bool exists = focused_ == "synth";
    for (auto& s : mfx) exists |= s.position == focused_;
    for (auto& s : fx) exists |= s.position == focused_;
    if (!exists) focused_ = "synth";
    chain_.setSlots(mfx, synth, fx, focused_);
    params_.setPosition(focused_ == "synth" && synth.moduleId.isEmpty() ? juce::String() : focused_);
    params_.reload();

    for (auto& s : mfx) { abbrevs.add(abbrev(s.moduleId.toStdString())); kinds.add(0); if (s.position == focused_) focusedIndex = abbrevs.size() - 1; }
    if (synth.moduleId.isNotEmpty()) { abbrevs.add(abbrev(synth.moduleId.toStdString())); kinds.add(1); if (focused_ == "synth") focusedIndex = abbrevs.size() - 1; }
    for (auto& s : fx) { abbrevs.add(abbrev(s.moduleId.toStdString())); kinds.add(2); if (s.position == focused_) focusedIndex = abbrevs.size() - 1; }
    ui::OledView::Content c;
    const auto* sel = focused_ == "synth" ? &synth : nullptr;
    for (auto& s : mfx) if (s.position == focused_) sel = &s;
    for (auto& s : fx) if (s.position == focused_) sel = &s;
    c.title = sel && sel->moduleId.isNotEmpty() ? sel->label : juce::String("Schwung");
    c.position = focused_ == "synth" ? "SYN" : focused_.startsWith("midi_fx") ? "MFX" + focused_.getLastCharacters(1)
                                                                               : "FX" + focused_.getLastCharacters(1);
    c.chainAbbrevs = abbrevs; c.chainKinds = kinds; c.focused = focusedIndex;
    c.patch = synth.moduleId.isEmpty() ? juce::String("Choose a synth below") : juce::String();
    lastParamName_.clear();
    oled_.setContent(c);
    seenRevision_ = proc_.chainRevision.load();
}

void SchwungEditor::focus(const juce::String& position) {
    focused_ = position;
    refreshChain();
}

void SchwungEditor::openBrowser(const juce::String& position, bool add) {
    const auto type = position == "synth" ? "sound_generator" : position.startsWith("midi_fx") ? "midi_fx" : "audio_fx";
    const juce::String current = add ? juce::String() : juce::String(proc_.engine().getParam(
        (position == "synth" ? std::string("synth_module") : (position + "_module").toStdString()), 128));
    browser_.onChoose = [this, position](const juce::String& id) {
        browser_.setVisible(false);
        if (id.isEmpty()) { if (chain_.onRemove) chain_.onRemove(position); }
        else { proc_.loadModule(position, id); focused_ = position; }
        // First synth in an empty slot: bind the DAW knobs to its page, the
        // way the device's knobs follow the module's own knob list.
        if (position == "synth" && id.isNotEmpty() && proc_.engine().getParam("knob_mapping_count", 8) == "0") {
            juce::var h = juce::JSON::parse(juce::String(proc_.engine().getParam("synth:ui_hierarchy", 1 << 18)));
            juce::var root = h["levels"]["root"];
            juce::var ks = root["knobs"];
            if (root["children"].isString() && !(ks.getArray() && ks.getArray()->size()))
                ks = h["levels"][juce::Identifier(root["children"].toString())]["knobs"];
            if (auto* a = ks.getArray())
                for (int i = 0; i < juce::jmin(8, a->size()); ++i)
                    proc_.engine().setParam("knob_" + std::to_string(i + 1) + "_set", "synth:" + (*a)[i].toString().toStdString());
        }
        refreshChain();
    };
    browser_.show(proc_.catalog().ofType(type), position, current);
    browser_.toFront(true);
}

void SchwungEditor::timerCallback() {
    if (proc_.chainRevision.load() != seenRevision_) refreshChain();
    status_.setText(proc_.statusLine(), juce::dontSendNotification);
    if (paramDirty_) { oled_.setParamLine(lastParamName_, lastParamValue_, lastParamNorm_); paramDirty_ = false; }
    static int tick = 0;
    if (++tick % 8 == 0) proc_.syncKnobsFromChain();
}
