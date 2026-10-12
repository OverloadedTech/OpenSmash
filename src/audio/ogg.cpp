#include "audio/ogg.h"
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

namespace sh {

struct OggStream::Impl {
    std::shared_ptr<const std::vector<uint8_t>> data; // stb_vorbis decodes the encoded bytes in place
    stb_vorbis* v = nullptr;
};

std::unique_ptr<OggStream> OggStream::open(std::vector<uint8_t> data) {
    return open(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
}

std::unique_ptr<OggStream> OggStream::open(std::shared_ptr<const std::vector<uint8_t>> data) {
    if (!data) return nullptr;
    std::unique_ptr<OggStream> s(new OggStream);
    s->d.reset(new Impl);
    s->d->data = std::move(data);
    int err = 0;
    s->d->v = stb_vorbis_open_memory(s->d->data->data(), (int)s->d->data->size(), &err, nullptr);
    if (!s->d->v) return nullptr;
    stb_vorbis_info info = stb_vorbis_get_info(s->d->v);
    s->rate_ = (int)info.sample_rate;
    s->channels_ = info.channels;
    return s;
}

OggStream::~OggStream() {
    if (d && d->v) stb_vorbis_close(d->v);
}

int OggStream::read(float* out, int frames) {
    return stb_vorbis_get_samples_float_interleaved(d->v, channels_, out, frames * channels_);
}
int OggStream::readStereo(float* out, int frames) {
    // stb writes exactly 2 floats per frame: the first two channels, a missing second one zero-filled
    int got = stb_vorbis_get_samples_float_interleaved(d->v, 2, out, frames * 2);
    if (channels_ == 1)
        for (int i = 0; i < got; i++) out[2 * i + 1] = out[2 * i];
    return got;
}
void OggStream::seekStart() { stb_vorbis_seek_start(d->v); }

} // namespace sh
