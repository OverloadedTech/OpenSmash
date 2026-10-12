#include "game/content.h"
#include <cstdio>
#include <cstdlib>

namespace sh {

Content::Content(const Vfs& vfs) : vfs_(vfs) {
    // materials.xml
    bool ok = false;
    std::string txt = vfs.readText("materials.xml", &ok);
    if (!ok) return;
    auto doc = parseXml(txt);
    if (doc) {
        if (const XmlNode* root = doc->child("materials"))
            for (auto& m : root->children) {
                Material mt;
                mt.density = m->floatAttr("density", 1);
                mt.friction = m->floatAttr("friction", 1);
                mt.restitution = m->floatAttr("restitution", 0);
                // roundness is only read when forcelimit is present, same as the original game
                if (m->attr("forcelimit")) {
                    mt.forceLimit = m->floatAttr("forcelimit", 0);
                    mt.roundness = m->floatAttr("roundness", 0);
                }
                if (const std::string* c = m->attr("color")) {
                    sscanf(c->c_str(), "%f %f %f", &mt.color[0], &mt.color[1], &mt.color[2]);
                    mt.color[3] = 1;
                }
                mt.category = (uint8_t)atoi(m->attrOr("category", "1").c_str());
                mt.mask = (uint8_t)atoi(m->attrOr("mask", "255").c_str());
                mt.isStatic = (uint8_t)atoi(m->attrOr("static", "0").c_str());
                std::string t = m->attrOr("type", "plain");
                mt.type = t == "marble"  ? kMaterialMarble
                          : t == "glass" ? kMaterialGlass
                          : t == "metal" ? kMaterialMetal
                                         : kMaterialPlain;
                materials_[m->attrOr("name", "")] = mt;
            }
    }
    // templates.xml: only the first child element of each <template> is read
    txt = vfs.readText("templates.xml", &ok);
    if (ok)
        if (auto t = parseXml(txt))
            if (const XmlNode* root = t->child("templates"))
                for (auto& tp : root->children) {
                    if (tp->name != "template" || tp->children.empty()) continue;
                    auto& dst = templates_[tp->attrOr("name", "")];
                    for (auto& a : tp->children[0]->attrs) dst[a.first] = a.second;
                }
    // game.xml
    txt = vfs.readText("game.xml", &ok);
    if (ok)
        if (auto g = parseXml(txt))
            if (const XmlNode* root = g->child("game"))
                if (const XmlNode* lv = root->child("levels"))
                    for (auto& l : lv->children) levelNames_.push_back(l->attrOr("name", ""));
    ok_ = !materials_.empty() && !levelNames_.empty();
}

const Material* Content::material(const std::string& name) const {
    auto it = materials_.find(name);
    return it == materials_.end() ? nullptr : &it->second;
}

const std::vector<Vec3>* Content::convex(const std::string& name) {
    auto it = convex_.find(name);
    if (it != convex_.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<Vec3> pts;
    bool ok = false;
    std::string txt = vfs_.readText("meshes/" + name + ".xml", &ok);
    if (ok)
        if (auto d = parseXml(txt))
            if (const XmlNode* r = d->child("convex"))
                for (auto& v : r->children) {
                    Vec3 p;
                    if (sscanf(v->text.c_str(), "%f %f %f", &p.x, &p.y, &p.z) == 3) pts.push_back(p);
                }
    if (pts.size() < 3) pts.clear();
    auto& slot = convex_[name];
    slot = pts;
    return slot.empty() ? nullptr : &slot;
}

std::string Content::prop(const XmlNode& n, const std::string& key, const std::string& def) const {
    if (const std::string* v = n.attr(key))
        if (!v->empty()) return *v;
    if (const std::string* t = n.attr("template")) {
        auto it = templates_.find(*t);
        if (it != templates_.end()) {
            auto k = it->second.find(key);
            if (k != it->second.end() && !k->second.empty()) return k->second;
        }
    }
    return def;
}

bool Content::loadLevelFile(const std::string& name, int levelIdx, int playerMode, bool allowBoss,
                            std::vector<RoomDef>& out, int* xmlTotal) const {
    bool ok = false;
    std::string txt = vfs_.readText("levels/" + name + ".xml", &ok);
    if (!ok) return false;
    auto doc = parseXml(txt);
    if (!doc) return false;
    const XmlNode* root = doc->child("level");
    if (!root) return false;
    for (auto& r : root->children) {
        if (r->name != "room") continue;
        RoomDef d;
        d.levelIdx = levelIdx;
        d.type = r->attrOr("type", "");
        const int len = atoi(r->attrOr("length", "0").c_str());
        if (xmlTotal) *xmlTotal += len; // counted before the boss filter
        if (d.type.find("boss") != std::string::npos && !(playerMode == kModeExpert || allowBoss))
            continue; // boss rooms: expert only
        d.length = len;
        for (auto& a : r->attrs)
            if (a.first != "type") d.params.emplace_back(a.first, a.second);
        out.push_back(std::move(d));
    }
    return true;
}

const SegmentDef* Content::segment(const std::string& name) {
    auto it = segments_.find(name);
    if (it != segments_.end()) return it->second ? it->second.get() : nullptr;
    auto& slot = segments_[name];
    bool ok = false;
    std::string txt = vfs_.readText("segments/" + name + ".xml", &ok);
    if (!ok) return nullptr;
    auto sd = std::make_unique<SegmentDef>();
    sd->doc = parseXml(txt);
    if (!sd->doc) return nullptr;
    sd->root = sd->doc->child("segment");
    if (!sd->root) return nullptr;
    std::string size = prop(*sd->root, "size", "20 5 20");
    sscanf(size.c_str(), "%f %f %f", &sd->size.x, &sd->size.y, &sd->size.z);
    slot = std::move(sd);
    return slot.get();
}

} // namespace sh
