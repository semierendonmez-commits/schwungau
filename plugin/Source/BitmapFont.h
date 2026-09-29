// Pixel-exact BDF font rendering for the OLED strip. Schwung draws Move's
// 128x64 display with the Tamzen bitmap fonts; so do we.
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <array>

class BitmapFont {
public:
    bool load(const char* bdfText, size_t size);
    int  height() const { return height_; }
    int  ascent() const { return ascent_; }
    int  textWidth(const juce::String& s) const;
    // Draws into an OLED framebuffer (1 byte per pixel, 0/1).
    int  draw(std::vector<uint8_t>& fb, int fbW, int fbH, int x, int y, const juce::String& s, bool on = true) const;
private:
    struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0, adv = 0; std::vector<uint32_t> rows; bool valid = false; };
    std::array<Glyph, 256> g_ {};
    int height_ = 0, ascent_ = 0;
};

// A monochrome display surface the size of Move's screen.
struct Oled {
    static constexpr int W = 128, H = 64;
    std::vector<uint8_t> px = std::vector<uint8_t>(W * H, 0);
    void clear() { std::fill(px.begin(), px.end(), 0); }
    void set(int x, int y, bool on = true) { if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)(y * W + x)] = on; }
    void rect(int x, int y, int w, int h, bool fill, bool on = true) {
        for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i)
            if (fill || j == 0 || j == h - 1 || i == 0 || i == w - 1) set(x + i, y + j, on);
    }
    void hline(int x, int y, int w, bool on = true) { for (int i = 0; i < w; ++i) set(x + i, y, on); }
    // Paint scaled with a faint pixel grid, like an OLED seen up close.
    void paint(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour on, juce::Colour off) const;
};
