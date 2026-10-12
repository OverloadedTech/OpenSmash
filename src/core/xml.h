// Minimal XML DOM sufficient for the game's data files (no DTD/namespaces/entities beyond the 5 basics).
// Accepts multiple top-level elements and files without prolog (original used rapidxml).
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace sh {

struct XmlNode {
    std::string name;
    std::string text; // concatenated character data directly inside this element (trimmed)
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    const std::string* attr(const std::string& k) const;
    std::string attrOr(const std::string& k, const std::string& def) const;
    float floatAttr(const std::string& k, float def) const;
    const XmlNode* child(const std::string& n) const;
};

// Parses `src`; the returned node is a synthetic root named "#document" holding all top-level elements.
std::unique_ptr<XmlNode> parseXml(const std::string& src, std::string* err = nullptr);

} // namespace sh
