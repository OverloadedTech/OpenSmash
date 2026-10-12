// Static game content loaded from the asset VFS: materials, templates, convex hulls, level playlists, segments.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/vfs.h"
#include "core/xml.h"
#include "physics/world.h"

namespace sh {

constexpr int kModeTraining = 0, kModeClassic = 1, kModeExpert = 2, kModeZen = 3, kModeVersus = 4, kModeCoop = 5;
constexpr int kModeCount = 6;

struct RoomDef {
    int levelIdx = 0;                                        // campaign level this room belongs to (checkpoint group)
    std::string type;                                        // rooms/<type>.lua
    std::vector<std::pair<std::string, std::string>> params; // every attribute except `type`
    int length = 0;                                          // nominal length for distance/score bookkeeping
};

struct SegmentDef {
    std::unique_ptr<XmlNode> doc;
    Vec3 size{20, 5, 20};
    const XmlNode* root = nullptr;
};

class Content {
public:
    explicit Content(const Vfs& vfs);
    const Vfs& vfs() const { return vfs_; }
    bool ok() const { return ok_; }

    const Material* material(const std::string& name) const;
    const std::vector<Vec3>* convex(const std::string& name);
    std::vector<std::string> levelNames() const { return levelNames_; } // game.xml order

    // Attribute lookup: a non-empty attribute, then the template's property, then the default.
    std::string prop(const XmlNode& n, const std::string& key, const std::string& def) const;
    // Builds the room playlist of levels/<name>.xml; `levelIdx` tags every RoomDef.
    // xmlTotal (optional): sum of every <room> length in the file, including skipped boss rooms
    bool loadLevelFile(const std::string& name, int levelIdx, int playerMode, bool allowBoss, std::vector<RoomDef>& out,
                       int* xmlTotal = nullptr) const;
    // Parsed segments/<name>.xml (cached), nullptr if missing/invalid.
    const SegmentDef* segment(const std::string& name);

private:
    const Vfs& vfs_;
    bool ok_ = false;
    std::map<std::string, Material> materials_;
    std::map<std::string, std::vector<Vec3>> convex_;
    std::map<std::string, std::map<std::string, std::string>> templates_;
    std::map<std::string, std::unique_ptr<SegmentDef>> segments_;
    std::vector<std::string> levelNames_;
};

} // namespace sh
