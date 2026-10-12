// Bitmap fonts (fonts/*.png + .fnt), unicode fonts (baked/*.ufnt) and localisation (locale/strings.txt).
#pragma once
#include <map>
#include <string>
#include <vector>
#include "core/vfs.h"

namespace sh {

struct TextMesh {
    std::vector<float> verts;  // x, y, u, v per vertex (4 per glyph)
    std::vector<uint32_t> idx; // 6 per glyph
    float width = 0, height = 0;
    int glyphs = 0;
};

class BitmapFont {
public:
    // type: 0 default 8x8 from 0x20, 1 numbers 4x4 from '0', 2 extended 16x16 from 0
    bool load(const Vfs& vfs, const std::string& name, int type, int texW, int texH);
    TextMesh build(const std::string& text) const;
    std::string pngPath;

private:
    int cols_ = 8, rows_ = 8, first_ = 0x20, type_ = 0;
    float cellW_ = 0, cellH_ = 0;
    std::vector<float> adv_;
};

class UnicodeFont {
public:
    bool load(const Vfs& vfs, const std::string& name, const std::string& lang);
    TextMesh buildLine(const std::u32string& text, float x, float y) const;
    TextMesh buildArea(const std::u32string& text, float width, int align /*0 center,1 left,2 right*/) const;
    float baseline() const { return baseline_; }
    std::string pngPath;
    int texW = 0, texH = 0; // filled by the owner after the texture loads
private:
    struct Glyph {
        int x, y, w, h;
        float xoff, yoff, adv;
    };
    float lineHeight_ = 40, baseline_ = 22, lineScaler_ = 2.0f;
    std::map<char32_t, int> map_;
    std::vector<Glyph> glyphs_;
    void emit(TextMesh& m, const Glyph& g, float penX, float penY) const;
};

class LocaleData {
public:
    void load(const Vfs& vfs, const std::string& lang);
    std::u32string translate(const std::string& key,
                             const std::vector<std::pair<std::string, std::string>>& args) const;

private:
    std::map<std::string, std::u32string> strings_;
};

std::u32string utf8ToU32(const std::string& s);
std::string toUpperAscii(std::string s);

} // namespace sh
