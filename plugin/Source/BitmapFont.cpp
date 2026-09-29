#include "BitmapFont.h"
#include <sstream>
#include <string>

bool BitmapFont::load(const char* text, size_t size) {
    std::istringstream in(std::string(text, size));
    std::string line;
    int enc = -1, fbbH = 0, fbbY = 0;
    Glyph cur;
    bool inBitmap = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k; ls >> k;
        if (k == "FONTBOUNDINGBOX") { int w; ls >> w >> fbbH; int x; ls >> x >> fbbY; height_ = fbbH; }
        else if (k == "FONT_ASCENT") ls >> ascent_;
        else if (k == "STARTCHAR") { cur = Glyph(); enc = -1; }
        else if (k == "ENCODING") ls >> enc;
        else if (k == "DWIDTH") ls >> cur.adv;
        else if (k == "BBX") ls >> cur.w >> cur.h >> cur.xoff >> cur.yoff;
        else if (k == "BITMAP") inBitmap = true;
        else if (k == "ENDCHAR") {
            inBitmap = false;
            cur.valid = true;
            if (enc >= 0 && enc < 256) g_[(size_t)enc] = cur;
        } else if (inBitmap) {
            cur.rows.push_back((uint32_t)std::stoul(k, nullptr, 16));
        }
    }
    if (ascent_ == 0) ascent_ = fbbH + fbbY;
    return g_[(size_t)'A'].valid;
}

int BitmapFont::textWidth(const juce::String& s) const {
    int w = 0;
    for (auto c : s) { auto& gl = g_[(size_t)((c < 256 && c >= 0) ? c : '?')]; w += gl.valid ? gl.adv : 0; }
    return w;
}

int BitmapFont::draw(std::vector<uint8_t>& fb, int W, int H, int x, int y, const juce::String& s, bool on) const {
    for (auto c : s) {
        const Glyph& gl = g_[(size_t)((c < 256 && c >= 0) ? c : '?')];
        if (!gl.valid) continue;
        const int bytes = (gl.w + 7) / 8;
        const int top = y + ascent_ - gl.yoff - gl.h;
        for (int r = 0; r < gl.h && r < (int)gl.rows.size(); ++r)
            for (int b = 0; b < gl.w; ++b)
                if ((gl.rows[(size_t)r] >> (bytes * 8 - 1 - b)) & 1u) {
                    const int px = x + gl.xoff + b, py = top + r;
                    if (px >= 0 && py >= 0 && px < W && py < H) fb[(size_t)(py * W + px)] = on;
                }
        x += gl.adv;
    }
    return x;
}

void Oled::paint(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour on, juce::Colour off) const {
    g.setColour(off);
    g.fillRect(r);
    const float sx = r.getWidth() / W, sy = r.getHeight() / H;
    const float gap = sx >= 3.f ? 0.6f : 0.f;      // visible pixel pitch when large
    g.setColour(on);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (px[(size_t)(y * W + x)])
                g.fillRect(r.getX() + x * sx, r.getY() + y * sy, sx - gap, sy - gap);
}
