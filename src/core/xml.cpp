#include "core/xml.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sh {

const std::string* XmlNode::attr(const std::string& k) const {
    for (auto& a : attrs)
        if (a.first == k) return &a.second;
    return nullptr;
}
std::string XmlNode::attrOr(const std::string& k, const std::string& def) const {
    auto p = attr(k);
    return p ? *p : def;
}
float XmlNode::floatAttr(const std::string& k, float def) const {
    auto p = attr(k);
    return p ? (float)atof(p->c_str()) : def;
}
const XmlNode* XmlNode::child(const std::string& n) const {
    for (auto& c : children)
        if (c->name == n) return c.get();
    return nullptr;
}

namespace {
struct P {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit P(const std::string& str) : s(str) {}
    bool eof() const { return i >= s.size(); }
    void ws() {
        while (!eof() && isspace((unsigned char)s[i])) i++;
    }
    static void unescape(std::string& t) {
        if (t.find('&') == std::string::npos) return;
        static const struct {
            const char* e;
            char c;
        } m[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
        std::string o;
        for (size_t k = 0; k < t.size();) {
            bool hit = false;
            if (t[k] == '&')
                for (auto& e : m) {
                    size_t n = strlen(e.e);
                    if (t.compare(k, n, e.e) == 0) {
                        o += e.c;
                        k += n;
                        hit = true;
                        break;
                    }
                }
            if (!hit) o += t[k++];
        }
        t = o;
    }
    bool skipMisc() { // comments, <? ?>, <!DOCTYPE>
        if (s.compare(i, 4, "<!--") == 0) {
            size_t e = s.find("-->", i + 4);
            if (e == std::string::npos) {
                err = "unterminated comment";
                return false;
            }
            i = e + 3;
            return true;
        }
        if (s.compare(i, 2, "<?") == 0) {
            size_t e = s.find("?>", i);
            if (e == std::string::npos) {
                err = "unterminated <?";
                return false;
            }
            i = e + 2;
            return true;
        }
        if (s.compare(i, 2, "<!") == 0) {
            size_t e = s.find('>', i);
            if (e == std::string::npos) {
                err = "unterminated <!";
                return false;
            }
            i = e + 1;
            return true;
        }
        return false;
    }
    static bool nameChar(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.'; }
    bool element(XmlNode& parent) {
        // at '<'
        i++;
        auto n = std::make_unique<XmlNode>();
        while (!eof() && nameChar(s[i])) n->name += s[i++];
        if (n->name.empty()) {
            err = "empty element name";
            return false;
        }
        for (;;) {
            ws();
            if (eof()) {
                err = "eof in tag " + n->name;
                return false;
            }
            if (s[i] == '/') {
                if (s.compare(i, 2, "/>") != 0) {
                    err = "bad '/'";
                    return false;
                }
                i += 2;
                parent.children.push_back(std::move(n));
                return true;
            }
            if (s[i] == '>') {
                i++;
                break;
            }
            std::string k, v;
            while (!eof() && nameChar(s[i])) k += s[i++];
            if (k.empty()) {
                err = "bad attribute in " + n->name;
                return false;
            }
            ws();
            if (eof() || s[i] != '=') {
                err = "expected '=' after " + k;
                return false;
            }
            i++;
            ws();
            if (eof() || (s[i] != '"' && s[i] != '\'')) {
                err = "unquoted attribute " + k;
                return false;
            }
            char q = s[i++];
            size_t e = s.find(q, i);
            if (e == std::string::npos) {
                err = "unterminated attribute " + k;
                return false;
            }
            v = s.substr(i, e - i);
            i = e + 1;
            unescape(v);
            n->attrs.emplace_back(k, v);
        }
        // content
        std::string text;
        for (;;) {
            if (eof()) {
                err = "eof inside <" + n->name + ">";
                return false;
            }
            if (s[i] == '<') {
                if (s.compare(i, 2, "</") == 0) {
                    size_t e = s.find('>', i);
                    if (e == std::string::npos) {
                        err = "bad close tag";
                        return false;
                    }
                    std::string cn = s.substr(i + 2, e - i - 2);
                    while (!cn.empty() && isspace((unsigned char)cn.back())) cn.pop_back();
                    if (cn != n->name) {
                        err = "mismatched </" + cn + "> for <" + n->name + ">";
                        return false;
                    }
                    i = e + 1;
                    break;
                }
                if (skipMisc()) continue;
                if (!err.empty()) return false;
                if (!element(*n)) return false;
            } else text += s[i++];
        }
        size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
        n->text = a == std::string::npos ? "" : text.substr(a, b - a + 1);
        unescape(n->text);
        parent.children.push_back(std::move(n));
        return true;
    }
};
} // namespace

std::unique_ptr<XmlNode> parseXml(const std::string& src, std::string* err) {
    auto root = std::make_unique<XmlNode>();
    root->name = "#document";
    P p(src);
    // skip UTF-8 BOM
    if (src.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;
    while (true) {
        p.ws();
        if (p.eof()) break;
        if (src[p.i] != '<') {
            if (err) *err = "text outside element at " + std::to_string(p.i);
            return nullptr;
        }
        if (p.skipMisc()) continue;
        if (!p.err.empty()) {
            if (err) *err = p.err;
            return nullptr;
        }
        if (!p.element(*root)) {
            if (err) *err = p.err;
            return nullptr;
        }
    }
    return root;
}

} // namespace sh
