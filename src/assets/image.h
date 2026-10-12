// Decodes the game's image formats (.png, .mtx = JPEG + optional zlib alpha plane) to RGBA8.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sh {
class Vfs;

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w*h*4, top row first
};

bool decodeImage(const Vfs& vfs, const std::string& name, Image& out, std::string* err = nullptr);
bool decodeImageMemory(const uint8_t* data, size_t size, bool isMtx, Image& out, std::string* err = nullptr);

} // namespace sh
