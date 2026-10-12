#include "ui/fonts.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace sh {

std::u32string utf8ToU32(const std::string& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 6) {
            cp = c & 0x1f;
            n = 2;
        } else if ((c >> 4) == 14) {
            cp = c & 0x0f;
            n = 3;
        } else if ((c >> 3) == 30) {
            cp = c & 0x07;
            n = 4;
        } else {
            i++;
            continue;
        }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3f);
        o.push_back(cp);
        i += n;
    }
    return o;
}

std::string toUpperAscii(std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// ------------------------------------------------------------ bitmap font

bool BitmapFont::load(const Vfs& vfs, const std::string& name, int type, int texW, int texH) {
    type_ = type;
    if (type == 1) {
        cols_ = rows_ = 4;
        first_ = 0x30;
    } else if (type == 2) {
        cols_ = rows_ = 16;
        first_ = 0;
    } else {
        cols_ = rows_ = 8;
        first_ = 0x20;
    }
    pngPath = "fonts/" + name + ".png";
    cellW_ = (float)texW / cols_;
    cellH_ = (float)texH / rows_;
    adv_.assign(cols_ * rows_, 0.0f);
    bool ok = false;
    std::string txt = vfs.readText("fonts/" + name + ".fnt", &ok);
    if (ok) {
        std::istringstream in(txt);
        std::string line;
        int i = 0;
        while (std::getline(in, line) && i < cols_ * rows_) {
            float v = (float)atof(line.c_str());
            adv_[i++] = v == 0.0f ? cellW_ : v;
        }
    }
    return ok;
}

TextMesh BitmapFont::build(const std::string& text) const {
    TextMesh m;
    std::string t = toUpperAscii(text);
    std::vector<int> codes;
    if (type_ == 2) {
        for (char32_t c : utf8ToU32(t))
            if (c < 256) codes.push_back((int)c);
    } else
        for (unsigned char c : t) codes.push_back(c);
    float pen = 0;
    for (int c : codes) {
        int idx = c - first_;
        if (idx < 0 || idx >= cols_ * rows_) continue;
        if (m.glyphs >= 1024) break;
        float u0 = (float)(idx % cols_) / cols_, v0 = (float)(idx / cols_) / rows_, u1 = u0 + 1.0f / cols_,
              v1 = v0 + 1.0f / rows_;
        float x0 = pen, x1 = pen + cellW_, y0 = 0, y1 = cellH_;
        float q[16] = {x0, y0, u0, v0, x0, y1, u0, v1, x1, y1, u1, v1, x1, y0, u1, v0};
        uint32_t base = (uint32_t)m.verts.size() / 4;
        m.verts.insert(m.verts.end(), q, q + 16);
        for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) m.idx.push_back(base + k);
        pen += adv_[idx];
        m.glyphs++;
    }
    m.width = pen;
    m.height = cellH_;
    return m;
}

// ------------------------------------------------------------ unicode font

bool UnicodeFont::load(const Vfs& vfs, const std::string& name, const std::string& lang) {
    std::string dir = (lang.empty() || lang == "en") ? "baked/" : "baked/localize_" + lang + "/";
    if (!vfs.exists(dir + name + ".ufnt")) dir = "baked/";
    pngPath = dir + name + ".png";
    // all or nothing: parse into locals and commit only once every line has been read, so a damaged or modded file
    // leaves the font empty instead of throwing out of mgCreateUText. The message is the same as the original's.
    auto fail = [&]() {
        fprintf(stderr, "Couldn't find data for font: %s\n", name.c_str());
        return false;
    };
    bool ok = false;
    std::string txt = vfs.readText(dir + name + ".ufnt", &ok);
    if (!ok) return fail();
    std::istringstream in(txt);
    float lineHeight = 0, baseline = 0;
    int n = 0;
    if (!(in >> lineHeight >> baseline >> n) || n < 0 || n > 65536) return fail();
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        int cp = 0, gi = 0;
        if (!(in >> cp >> gi)) return fail();
        pairs.push_back({cp, gi});
    }
    std::vector<Glyph> glyphs((size_t)n);
    // atlas rect as x0 y0 x1 y1 (exclusive end), then offsets and advance
    // the file writes every number as a decimal ("2.0"), so read floats even for the integer pixel rect
    for (int i = 0; i < n; i++) {
        Glyph& g = glyphs[i];
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!(in >> x0 >> y0 >> x1 >> y1 >> g.xoff >> g.yoff >> g.adv)) return fail();
        g.x = (int)x0;
        g.y = (int)y0;
        g.w = (int)(x1 - x0);
        g.h = (int)(y1 - y0);
    }
    lineHeight_ = lineHeight;
    baseline_ = baseline;
    glyphs_ = std::move(glyphs);
    map_.clear();
    for (auto& p : pairs)
        if (!map_.count((char32_t)p.first) && p.second >= 0 && p.second < n) map_[(char32_t)p.first] = p.second;
    return true;
}

void UnicodeFont::emit(TextMesh& m, const Glyph& g, float penX, float penY) const {
    float left = std::floor(penX + g.xoff + 0.5f), top = std::floor(penY + g.yoff + 0.5f);
    float right = left + g.w, bottom = top + g.h;
    float tw = texW > 0 ? (float)texW : 1.0f, th = texH > 0 ? (float)texH : 1.0f;
    float u0 = g.x / tw, v0 = g.y / th, u1 = (g.x + g.w) / tw, v1 = (g.y + g.h) / th;
    float q[16] = {left, top, u0, v0, left, bottom, u0, v1, right, bottom, u1, v1, right, top, u1, v0};
    uint32_t base = (uint32_t)m.verts.size() / 4;
    m.verts.insert(m.verts.end(), q, q + 16);
    for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) m.idx.push_back(base + k);
    m.glyphs++;
    m.height = std::max(m.height, (float)g.h);
}

TextMesh UnicodeFont::buildLine(const std::u32string& text, float x, float y) const {
    TextMesh m;
    float pen = x;
    for (char32_t c : text) {
        auto it = map_.find(c);
        if (it == map_.end()) continue;
        const Glyph& g = glyphs_[it->second];
        emit(m, g, pen, y);
        pen += g.adv;
    }
    m.width = pen - x;
    return m;
}

TextMesh UnicodeFont::buildArea(const std::u32string& text, float width, int align) const {
    TextMesh m;
    auto adv = [&](char32_t c) {
        auto it = map_.find(c);
        return it == map_.end() ? 0.0f : glyphs_[it->second].adv;
    };
    float spaceW = adv(U' ');
    // word breaks, consumed rather than drawn: space, U+30FB katakana middle dot, U+3002 ideographic full stop and
    // U+4E00 (the CJK ideograph for 'one')
    std::vector<std::u32string> words;
    std::u32string cur;
    for (char32_t c : text) {
        if (c == U' ' || c == 0x30FB || c == 0x3002 || c == 0x4E00) {
            if (!cur.empty()) words.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    if (!cur.empty()) words.push_back(cur);
    struct Line {
        std::vector<std::u32string> w;
        float width = 0;
    };
    std::vector<Line> lines(1);
    for (auto& w : words) {
        float ww = 0;
        for (char32_t c : w) ww += adv(c);
        if (lines.back().width + ww > width && !lines.back().w.empty()) lines.emplace_back();
        lines.back().w.push_back(w);
        lines.back().width += ww + spaceW;
    }
    float y = 0, step = lineHeight_ * lineScaler_;
    for (auto& ln : lines) {
        float x = align == 0 ? (width - ln.width) / 2 : align == 2 ? width - ln.width : 0.0f;
        for (auto& w : ln.w) {
            for (char32_t c : w) {
                auto it = map_.find(c);
                if (it == map_.end()) continue;
                emit(m, glyphs_[it->second], x, y);
                x += glyphs_[it->second].adv;
            }
            x += spaceW;
        }
        y += step;
    }
    m.width = width;
    return m;
}

// ------------------------------------------------------------ locale

void LocaleData::load(const Vfs& vfs, const std::string& lang) {
    strings_.clear();
    std::string path = "locale/strings.txt";
    if (!lang.empty() && lang != "en" && vfs.exists("locale/localize_" + lang + "/strings.txt"))
        path = "locale/localize_" + lang + "/strings.txt";
    std::istringstream in(vfs.readText(path));
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key;
        for (size_t i = 0; i < eq; i++) {
            unsigned char c = line[i];
            if (isalpha(c) || c == '_' || c == '-') key += (char)c;
        }
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        strings_[key] = utf8ToU32(val);
    }
}

std::u32string LocaleData::translate(const std::string& key,
                                     const std::vector<std::pair<std::string, std::string>>& args) const {
    auto it = strings_.find(key);
    if (it == strings_.end()) return U"";
    std::u32string s = it->second;
    for (auto& a : args) {
        std::u32string pat = utf8ToU32("{" + a.first + "}"), rep = utf8ToU32(a.second);
        for (size_t p = 0; (p = s.find(pat, p)) != std::u32string::npos; p += rep.size()) s.replace(p, pat.size(), rep);
    }
    return s;
}

} // namespace sh
