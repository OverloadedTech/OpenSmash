// Streaming Ogg Vorbis decoder (stb_vorbis) producing interleaved float stereo at the file's sample rate.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace sh {

class OggStream {
public:
    // Takes ownership of the encoded bytes (stb_vorbis reads from memory).
    static std::unique_ptr<OggStream> open(std::vector<uint8_t> data);
    // Decodes from shared bytes (kept alive by the stream): the audio thread reopens cached boss loops without a copy.
    static std::unique_ptr<OggStream> open(std::shared_ptr<const std::vector<uint8_t>> data);
    ~OggStream();
    int sampleRate() const { return rate_; }
    int channels() const { return channels_; }
    // Decode up to `frames` frames into `out` (interleaved, `channels()` per frame). Returns frames written (0 at end).
    int read(float* out, int frames);
    // Same, but always 2 floats per frame whatever the file's channel count: channels 0 and 1, mono duplicated.
    int readStereo(float* out, int frames);
    void seekStart();

private:
    OggStream() = default;
    struct Impl;
    std::unique_ptr<Impl> d;
    int rate_ = 0, channels_ = 0;
};

} // namespace sh
