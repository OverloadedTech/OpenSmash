// Virtual file system over a user-supplied APK (zip) or extracted asset folder.
// The original APK stores every asset with an extra ".mp3" suffix; lookups
// accept the plain name and transparently try "<name>.mp3".
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sh {

class Vfs {
public:
    Vfs();
    ~Vfs();
    // Mount an .apk/.zip (assets under "assets/"), an extracted asset folder, or a plainly unzipped APK, in which case
    // its assets/ subtree is used. Returns false and sets err on failure. Does not check what the data is; see
    // mountGame.
    bool mount(const std::string& path, std::string* err = nullptr);
    bool exists(const std::string& name) const;
    // Thread-safe: reads from several threads are serialised on the APK's single zip reader.
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    std::string readText(const std::string& name, bool* ok = nullptr) const;
    // All files below a directory prefix (recursive), names without ".mp3" suffix, sorted.
    std::vector<std::string> list(const std::string& dirPrefix) const;
    bool mounted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// True if the mounted data is Smash Hit's (game.xml, materials.xml and shaders/room.glsl resolve); else names the first
// missing file in *missing. A foreign APK (most have an assets/ folder) or an unrelated folder fails here.
bool hasGameData(const Vfs& v, std::string* missing = nullptr);
// Vfs::mount plus hasGameData, with a message for the player on failure. The asset search, --assets / SH_ASSETS and
// the headless runner all go through it, so a wrong source is refused before it is remembered or reaches the renderer.
bool mountGame(Vfs& v, const std::string& path, std::string* err = nullptr);

} // namespace sh
