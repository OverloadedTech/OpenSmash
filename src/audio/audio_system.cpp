// SDL3 software mixer: SFX voices (world + UI pools), two-slot level-music stream with the 32 s overlap and
// music clock, looping menu music, and the per-room reverb/echo/low-pass chain.
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include "audio/audio.h"
#include "audio/ogg.h"
#include "core/math.h"
#include "core/vfs.h"

namespace sh {

namespace {

constexpr int kRate = 44100;
constexpr int kSwitchFrames = 1411200;      // 32 s at 44.1 kHz
constexpr float kIntroSkip = 2.0f;          // seconds decoded and dropped at the start of a level track
constexpr float kMusicClockLatency = 0.15f; // the music clock reads this far behind the frames handed to SDL

inline float mix2(float a, float b) { return a + b - std::fabs(a) * b; }

// ------------------------------------------------------------------ effects (per channel, mono)

struct LowPass {
    float coef = 0, s = 0;
    float process(float x, float mix) {
        if (mix * coef <= 0) return x;
        s = coef * s + (1 - coef) * x;
        return (1 - mix) * x + mix * s;
    }
};

struct Echo {
    float wet = 0, damp = 0, fb = 0;
    std::vector<float> buf;
    int len = 0, r = 0;
    float lp = 0;
    void init(float w, float delay, float feedback, float d) {
        wet = w;
        fb = feedback;
        damp = d;
        len = (int)clampf(delay * kRate, 0, (float)kRate);
        buf.assign(std::max(len, 1), 0.0f);
    }
    float process(float x, float mix) {
        if (mix * wet <= 0 || len <= 0) return x;
        lp = damp * lp + (1 - damp) * buf[r];
        buf[(r + len - 1) % len] = (lp + x) * fb;
        r = (r + 1) % len;
        return x + mix * wet * lp;
    }
};

struct Reverb {
    float g = 0;
    std::vector<float> er, comb[4], a1, a2, pre;
    int erP = 0, cl[4], ci[4], aj = 0, am = 0, q = 0, preLen = 1;
    float fbk[4], ak[4], st[4] = {0, 0, 0, 0};
    float g1 = 0.6123504f, g2 = 0.6934258f;
    bool active = false;
    void init(float gain, float T, float damp, std::mt19937& rng) {
        g = gain;
        active = gain > 0;
        if (!active) return;
        float rt60 = 4.5f * std::max(T, 0.01f);
        preLen = std::max(1, (int)clampf(0.1f * T * kRate, 0, 8192));
        pre.assign(preLen, 0.0f);
        er.assign(16383, 0.0f);
        static const float ct[4] = {0.058f, 0.065f, 0.071f, 0.077f}, dm[4] = {1.0f, 1.17f, 1.43f, 1.64f};
        std::uniform_real_distribution<float> u(0.8f, 1.2f);
        for (int k = 0; k < 4; k++) {
            float t = ct[k] * u(rng);
            cl[k] = std::max(1, (int)(kRate * t));
            comb[k].assign(cl[k], 0.0f);
            ci[k] = 0;
            fbk[k] = std::pow(0.001f, t / rt60);
            ak[k] = clampf(damp * dm[k], 0, 1);
        }
        a1.assign(313, 0.0f);
        a2.assign(233, 0.0f);
    }
    float process(float x, float mix) {
        if (!active || mix * g <= 0) return x;
        static const int d[4] = {366, 992, 1190, 2019};
        static const float e[4] = {0.3f * 0.84f, 0.3f * 0.49f, 0.3f * 0.38f, 0.3f * 0.29f};
        const int N = 16383;
        for (int k = 0; k < 4; k++) er[(erP + d[k]) % N] += x * e[k];
        float y = x + er[erP];
        er[erP] = 0; // consume the slot just read
        erP = (erP + 1) % N;
        float v[4], c = 0;
        for (int k = 0; k < 4; k++) {
            v[k] = comb[k][ci[k]];
            float s = ak[k] * st[k] + (1 - ak[k]) * (fbk[k] * v[k] + y);
            st[k] = s;
            comb[k][ci[k]] = s;
            ci[k] = (ci[k] + 1) % cl[k];
            c += v[k];
        }
        c *= 0.25f;
        float w = a1[aj];
        a1[aj] = w * g1 + c;
        float c1 = w - c * g1;
        aj = (aj + 1) % 313;
        w = a2[am];
        a2[am] = w * g2 + c1;
        float c2 = w - c1 * g2;
        am = (am + 1) % 233;
        pre[(q + preLen - 1) % preLen] = c2;
        float late = pre[q];
        q = (q + 1) % preLen;
        return (1 - mix) * x + mix * (g * late + y);
    }
};

struct SoundFilter {
    float amount = 0;
    LowPass lp[2];
    Echo ec[2];
    Reverb rv[2];
    void init(float lowpass, float ea, float eb, float ec_, float ed, float ra, float rb, float rc, std::mt19937& rng) {
        for (int c = 0; c < 2; c++) {
            lp[c].coef = lowpass;
            ec[c].init(ea, eb, ec_, ed);
            rv[c].init(ra, rb, rc, rng);
        }
    }
    void process(float* L, float* R, int n) {
        for (int i = 0; i < n; i++) {
            L[i] = rv[0].process(ec[0].process(lp[0].process(L[i], amount), amount), amount);
            R[i] = rv[1].process(ec[1].process(lp[1].process(R[i], amount), amount), amount);
        }
    }
};

// ------------------------------------------------------------------ sounds / voices

struct Sound {
    std::vector<float> frames; // interleaved stereo
    int n = 0;
};

struct Voice {
    std::shared_ptr<Sound> s;
    double pos = 0;
    float vol = 1, pitch = 1, pan = 0;
    bool active = false;
    uint64_t stamp = 0;
};

// Level music: two decoder slots, 32 s switch point, played/decoded frame counters
struct LevelMusic {
    struct Slot {
        std::unique_ptr<OggStream> dec;
        int64_t decoded = 0, played = 0;
        bool active = false;
    } slot[2];
    int cur = 0;
    bool nextQueued = false;
    bool valid = false;

    static bool open(Slot& s, const Vfs& vfs, const std::string& path) {
        std::vector<uint8_t> data;
        if (!vfs.read(path, data)) return false;
        s.dec = OggStream::open(std::move(data));
        s.decoded = s.played = 0;
        s.active = s.dec != nullptr;
        // the level-music stream is always 44.1 kHz stereo, as in the original game. A file at another rate plays
        // at the wrong speed, and the music clock counts the frames actually heard.
        if (s.dec && s.dec->sampleRate() != kRate)
            fprintf(stderr, "%s: %d Hz, played at %d Hz (level music is a fixed 44.1 kHz stream)\n", path.c_str(),
                    s.dec->sampleRate(), kRate);
        return s.active;
    }
    // stereo frames, any channel count
    int decode(Slot& s, float* out, int n) { return s.dec ? s.dec->readStereo(out, n) : 0; }
    void start(const Vfs& vfs, const std::string& path) {
        slot[0] = Slot();
        slot[1] = Slot();
        cur = 0;
        nextQueued = false;
        valid = open(slot[0], vfs, path);
        if (valid) {
            // decode and discard the intro, counting it as played
            std::vector<float> tmp(4096 * 2);
            int left = (int)(kRate * kIntroSkip);
            while (left > 0) {
                int got = decode(slot[0], tmp.data(), std::min(4096, left));
                if (got <= 0) break;
                left -= got;
                slot[0].decoded += got;
            }
            slot[0].played = slot[0].decoded;
        }
    }
    void queueNext(const Vfs& vfs, const std::string& path) {
        int o = 1 - cur;
        slot[o] = Slot();
        nextQueued = open(slot[o], vfs, path);
        slot[o].active = false; // becomes active when the current track passes 32 s
    }
    void swap() {
        cur = 1 - cur;
        nextQueued = false;
    }
    float location() const { return valid ? (float)slot[cur].played / kRate : -1.0f; }
    // Produces n stereo frames of the mixed streams.
    void getData(float* out, int n) {
        std::fill(out, out + 2 * n, 0.0f);
        if (!valid) return;
        int got = 0;
        Slot& c = slot[cur];
        Slot& o = slot[1 - cur];
        if (c.active) {
            got = decode(c, out, n);
            c.decoded += got;
            if (got < n) c.active = false;
        }
        int startOff = 0;
        if (!o.active && nextQueued && o.dec) {
            // the switch frame lies in this chunk, [decoded - got, decoded). excess == got has to count as well,
            // because 10 ms callbacks (441 or 882 frames, e.g. WASAPI) can end exactly on kSwitchFrames.
            int64_t excess = c.decoded - kSwitchFrames;
            if (excess > 0 && excess <= got) {
                o.active = true;
                startOff = got - (int)excess;
            }
        }
        if (o.active) {
            int want = std::max(0, n - startOff);
            std::vector<float> tmp(want * 2 + 2);
            int g2 = decode(o, tmp.data(), want);
            o.decoded += g2;
            if (g2 < want) o.active = false;
            for (int i = 0; i < g2; i++) {
                out[2 * (startOff + i)] = mix2(out[2 * (startOff + i)], tmp[2 * i]);
                out[2 * (startOff + i) + 1] = mix2(out[2 * (startOff + i) + 1], tmp[2 * i + 1]);
            }
        }
    }
    void reportPlayed(int n) {
        Slot& c = slot[cur];
        c.played += n;
        if (c.played > kSwitchFrames) {
            int64_t ov = std::min<int64_t>(c.played - kSwitchFrames, n);
            slot[1 - cur].played += ov;
        }
        c.played = std::min(c.played, std::max<int64_t>(c.decoded, 0));
    }
};

struct BgMusic {
    std::unique_ptr<OggStream> dec;
    std::string name;
    float vol = 0;
    bool active = false;
};

struct BossStream {
    std::unique_ptr<OggStream> dec;
    std::string cur, next;
    int rate = kRate; // the streaming buffer keeps the rate of the file playBossLoop opened
    bool cont = false, nextCont = false, playing = false;
};

class SdlAudio : public AudioSystem {
public:
    explicit SdlAudio(const Vfs& vfs) : vfs_(vfs) {}
    ~SdlAudio() override {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

    bool open() {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = kRate;
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudio::callback, this);
        if (!stream_) return false;
        filter_.reset(new SoundFilter);
        filter_->amount = 1;
        filter_->init(0, 0, 0, 0, 0, 0, 0, 0, rng_);
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    int load(const std::string& path) override {
        std::lock_guard<std::mutex> lk(m_);
        auto it = index_.find(path);
        if (it != index_.end()) return it->second;
        std::vector<uint8_t> data;
        int h = -1;
        if (vfs_.read(path, data)) {
            auto dec = OggStream::open(std::move(data));
            if (dec) {
                auto snd = std::make_shared<Sound>();
                std::vector<float> buf(4096 * 2);
                for (;;) {
                    int got = dec->readStereo(buf.data(), 4096);
                    if (got <= 0) break;
                    snd->frames.insert(snd->frames.end(), buf.begin(), buf.begin() + 2 * got);
                }
                snd->n = (int)snd->frames.size() / 2;
                // resample once to 44100 (nearest) if needed
                if (dec->sampleRate() != kRate && snd->n > 0) {
                    double ratio = (double)dec->sampleRate() / kRate;
                    int nn = (int)(snd->n / ratio);
                    std::vector<float> rs(nn * 2);
                    for (int i = 0; i < nn; i++) {
                        int si = std::min(snd->n - 1, (int)(i * ratio));
                        rs[2 * i] = snd->frames[2 * si];
                        rs[2 * i + 1] = snd->frames[2 * si + 1];
                    }
                    snd->frames.swap(rs);
                    snd->n = nn;
                }
                sounds_.push_back(snd);
                h = (int)sounds_.size() - 1;
            }
        }
        index_[path] = h;
        return h;
    }

    void play(int handle, float vol, float pitch, float pan) override {
        std::lock_guard<std::mutex> lk(m_);
        if (handle < 0 || handle >= (int)sounds_.size() || soundVol_ <= 0) return;
        startVoice(voices_, handle, vol, pitch)->pan = clampf(pan, -1, 1);
    }

    // UI sounds from menu and HUD scripts: a small separate pool, mixed dry, never slowed down or paused
    void playUi(int handle, float vol, float pitch) override {
        std::lock_guard<std::mutex> lk(m_);
        if (handle < 0 || handle >= (int)sounds_.size() || soundVol_ <= 0) return;
        startVoice(uiVoices_, handle, vol, pitch)->pan = 0;
    }

    void playLevelMusic(const std::string& name) override {
        std::lock_guard<std::mutex> lk(m_);
        level_.start(vfs_, "music/" + name + ".ogg");
        bg_.active = false;
    }
    void queueNextLevelMusic(const std::string& name) override {
        std::lock_guard<std::mutex> lk(m_);
        level_.queueNext(vfs_, "music/" + name + ".ogg");
    }
    void swapLevelMusic() override {
        std::lock_guard<std::mutex> lk(m_);
        level_.swap();
    }
    bool levelMusicContinues() override {
        std::lock_guard<std::mutex> lk(m_);
        return level_.valid && level_.slot[1 - level_.cur].active;
    }
    void playBossLoop(const std::string& path) override {
        cacheBoss(path);
        std::lock_guard<std::mutex> lk(m_);
        boss_.cur = boss_.next = path;
        boss_.cont = boss_.nextCont = true;
        boss_.dec = openBoss(path);
        boss_.rate = boss_.dec ? boss_.dec->sampleRate() : kRate;
        boss_.playing = boss_.dec != nullptr;
    }
    void setNextBossLoop(const std::string& path, bool once) override {
        cacheBoss(path);
        std::lock_guard<std::mutex> lk(m_);
        boss_.next = path;
        boss_.nextCont = !once;
    }
    bool bossLoopPlaying() override {
        std::lock_guard<std::mutex> lk(m_);
        return boss_.playing;
    }
    std::string bossLoopName() override {
        std::lock_guard<std::mutex> lk(m_);
        return boss_.cur;
    }
    void stopBossLoop() override {
        std::lock_guard<std::mutex> lk(m_);
        boss_ = BossStream();
    }
    void stopLevelStream() override {
        std::lock_guard<std::mutex> lk(m_);
        level_ = LevelMusic();
    }
    void stopLevelMusic() override {
        std::lock_guard<std::mutex> lk(m_);
        boss_ = BossStream();
        level_ = LevelMusic();
        for (auto& v : voices_) v.active = false;
    }
    float levelMusicLocation() override {
        std::lock_guard<std::mutex> lk(m_);
        if (musicVol_ <= 0) return -1.0f; // music off: the level clock free-runs
        if (!level_.valid) return 0.0f;   // no stream: 0 holds the level clock
        return level_.location() - kMusicClockLatency - kIntroSkip;
    }
    bool musicEnabled() override { return musicVol_ > 0; } // only the game thread writes musicVol_ (setVolumes)

    void playBackgroundMusic(const std::string& path) override {
        std::lock_guard<std::mutex> lk(m_);
        if (musicVol_ <= 0) return; // only with music enabled
        if (bg_.active && bg_.name == path) return;
        std::vector<uint8_t> data;
        if (!vfs_.read(path, data)) return;
        bg_.dec = OggStream::open(std::move(data));
        bg_.name = path;
        bg_.active = bg_.dec != nullptr;
        bg_.vol = 0;
    }
    void stopBackgroundMusic() override {
        std::lock_guard<std::mutex> lk(m_);
        bg_.active = false;
        bg_.dec.reset();
        bg_.name.clear();
    }

    void setReverb(float lp, float a, float b, float c, float e0, float e1, float e2, float e3) override {
        std::lock_guard<std::mutex> lk(m_);
        // reverb is (gain, time, damp) in a, b, c and echo is (wet, delay, feedback, damp) in e0..e3
        auto nf = std::make_unique<SoundFilter>();
        nf->init(lp, e0, e1, e2, e3, a, b, c, rng_);
        nf->amount = 0;
        oldFilter_ = std::move(filter_);
        filter_ = std::move(nf);
        fadeT_ = 0;
    }
    void setVolumes(float music, float sfx) override {
        std::lock_guard<std::mutex> lk(m_);
        musicVol_ = clampf(music * 0.7f, 0, 10);
        soundVol_ = clampf(sfx, 0, 10);
        // disabling music drops the menu track (the original fades it out under a zero gain, so nothing audible is
        // lost). Re-enabling starts the track from the beginning.
        if (musicVol_ <= 0) {
            bg_.active = false;
            bg_.dec.reset();
            bg_.name.clear();
        }
    }
    // world time scale: drives pitch and volume of the level music, boss music and world SFX. At 0.01 or below
    // they are silent and frozen.
    void setPitch(float p) override {
        std::lock_guard<std::mutex> lk(m_);
        pitch_ = clampf(p, 0, 10);
    }
    void setPaused(bool p) override {
        std::lock_guard<std::mutex> lk(m_);
        paused_ = p;
    }
    void setLoop(int slot, int handle, float vol, float pan) override {
        std::lock_guard<std::mutex> lk(m_);
        if (slot < 0 || slot >= 3) return;
        Voice& v = loops_[slot];
        if (vol <= 0 || handle < 0 || handle >= (int)sounds_.size()) {
            v.active = false;
            return;
        }
        if (!v.active || v.s != sounds_[handle]) {
            v.s = sounds_[handle];
            v.pos = 0;
            v.active = true;
        }
        v.vol = clampf(vol, 0, 10);
        v.pan = clampf(pan, -1, 1);
        v.pitch = 1;
    }
    void setFlanger(float amount) override {
        std::lock_guard<std::mutex> lk(m_);
        flangeAmt_ = amount;
    }

private:
    const Vfs& vfs_;
    SDL_AudioStream* stream_ = nullptr;
    std::mutex m_;
    std::map<std::string, int> index_;
    std::vector<std::shared_ptr<Sound>> sounds_;
    Voice voices_[16];
    Voice loops_[3]; // looping channels: laser hum, ball roll, object slide
    Voice uiVoices_[4];
    float flangeAmt_ = 0; // flanger on the level-music channel
    float flangeRing_[2][4096] = {};
    int flangeW_ = 0;
    uint64_t stamp_ = 0;
    LevelMusic level_;
    BgMusic bg_;
    BossStream boss_;
    // Encoded boss-loop files (a few ~42 KB oggs), read on the game thread and kept for the whole session so the audio
    // thread can reopen the next loop without touching the VFS. Missing files are cached as null since
    // setNextBossLoop runs every frame.
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> bossBytes_;
    void cacheBoss(const std::string& path) { // game thread; the file read happens outside the lock
        {
            std::lock_guard<std::mutex> lk(m_);
            if (bossBytes_.count(path)) return;
        }
        auto data = std::make_shared<std::vector<uint8_t>>();
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        if (vfs_.read(path, *data)) bytes = std::move(data);
        std::lock_guard<std::mutex> lk(m_);
        bossBytes_.emplace(path, std::move(bytes));
    }
    std::unique_ptr<OggStream> openBoss(const std::string& path) { // m_ held; cached bytes only
        auto it = bossBytes_.find(path);
        return it != bossBytes_.end() ? OggStream::open(it->second) : nullptr;
    }
    // m_ held: starts sound `handle` on the first free voice of the pool, else on the one started longest ago
    template <size_t N> Voice* startVoice(Voice (&pool)[N], int handle, float vol, float pitch) {
        Voice* best = nullptr;
        for (auto& v : pool)
            if (!v.active) {
                best = &v;
                break;
            }
        if (!best) {
            best = &pool[0];
            for (auto& v : pool)
                if (v.stamp < best->stamp) best = &v;
        }
        best->s = sounds_[handle];
        best->pos = 0;
        best->active = true;
        best->stamp = ++stamp_;
        best->vol = clampf(vol, 0, 10);
        best->pitch = clampf(pitch, 0, 10);
        return best;
    }
    std::unique_ptr<SoundFilter> filter_, oldFilter_;
    float fadeT_ = 1;
    float musicVol_ = 0.7f, soundVol_ = 1.0f, pitch_ = 1.0f;
    bool paused_ = false;
    std::mt19937 rng_{99};

    static void SDLCALL callback(void* ud, SDL_AudioStream* stream, int additional, int) {
        static_cast<SdlAudio*>(ud)->fill(stream, additional);
    }

    void fill(SDL_AudioStream* stream, int additionalBytes) {
        int frames = additionalBytes / 8;
        std::vector<float> out(std::min(frames, 2048) * 2);
        while (frames > 0) {
            int n = std::min(frames, 2048);
            mixChunk(out.data(), n);
            SDL_PutAudioStreamData(stream, out.data(), n * 8);
            frames -= n;
        }
    }

    // SFX voices
    void mixVoices(float* sL, float* sR, int n, float rateScale) {
        for (auto& v : voices_) {
            if (!v.active) continue;
            float gl = (1 - std::max(v.pan, 0.0f)) * v.vol, gr = (1 + std::min(v.pan, 0.0f)) * v.vol;
            double step = v.pitch * rateScale;
            for (int i = 0; i < n; i++) {
                int idx = (int)v.pos;
                if (idx >= v.s->n) {
                    v.active = false;
                    break;
                }
                sL[i] = mix2(sL[i], gl * v.s->frames[2 * idx]);
                sR[i] = mix2(sR[i], gr * v.s->frames[2 * idx + 1]);
                v.pos += step;
            }
        }
    }

    // SoundLoop channels (looping, reverb chain like the one-shots)
    void mixLoops(float* sL, float* sR, int n, float rateScale) {
        for (auto& v : loops_) {
            if (!v.active || !v.s || v.s->n <= 0) continue;
            float gl = (1 - std::max(v.pan, 0.0f)) * v.vol, gr = (1 + std::min(v.pan, 0.0f)) * v.vol;
            for (int i = 0; i < n; i++) {
                int idx = (int)v.pos % v.s->n;
                sL[i] = mix2(sL[i], gl * v.s->frames[2 * idx]);
                sR[i] = mix2(sR[i], gr * v.s->frames[2 * idx + 1]);
                v.pos += rateScale;
                if (v.pos >= v.s->n) v.pos -= v.s->n;
            }
        }
    }

    // room filter chain with crossfade (while the world is off, the effect tails freeze too)
    void filterWorld(float* sL, float* sR, int n, bool worldOff) {
        if (oldFilter_) {
            oldFilter_->amount = 1 - fadeT_;
            if (!worldOff) oldFilter_->process(sL, sR, n);
        }
        if (filter_) {
            filter_->amount = fadeT_ >= 1 && !oldFilter_ ? 1.0f : fadeT_;
            if (!worldOff) filter_->process(sL, sR, n);
        }
        fadeT_ = std::min(1.0f, fadeT_ + (float)n / kRate);
        if (fadeT_ >= 1 && oldFilter_) {
            oldFilter_.reset();
            filter_->amount = 1;
        }
    }

    // level music (pitch and volume follow slow-motion / fail fade / pause fade; clock follows consumed frames)
    void mixLevelMusic(float* mL, float* mR, int n, float tsGain) {
        float step = pitch_;
        int need = std::max(1, (int)std::ceil(n * step));
        std::vector<float> raw(need * 2 + 2);
        for (int off = 0; off < need;) {
            int c = std::min(4096, need - off);
            level_.getData(&raw[off * 2], c);
            off += c;
        }
        const int D = (int)std::floor(flangeAmt_ * 120.0f + 5.0f);
        for (int i = 0; i < n; i++) {
            int j = std::min(need - 1, (int)std::floor(i * step + 0.5f));
            float x[2] = {raw[2 * j] * tsGain, raw[2 * j + 1] * tsGain}; // channel gain before its flanger
            // mono flanger per channel: y = 0.5x + 0.9*delayed, ring = 0.4x + 0.6y.
            // When off, the ring just records x.
            flangeW_ = (flangeW_ + 1) & 4095;
            for (int c = 0; c < 2; c++) {
                if (flangeAmt_ > 0) {
                    float d = flangeRing_[c][(flangeW_ - D) & 4095];
                    float y = 0.5f * x[c] + 0.9f * d;
                    flangeRing_[c][flangeW_] = 0.4f * x[c] + 0.6f * y;
                    x[c] = y;
                } else flangeRing_[c][flangeW_] = x[c];
            }
            mL[i] = mix2(mL[i], x[0]);
            mR[i] = mix2(mR[i], x[1]);
        }
        level_.reportPlayed(need);
    }

    // boss loop stream (pitch and volume follow the time scale like the level music)
    void mixBossMusic(float* mL, float* mR, int n, float tsGain) {
        // streaming buffer at the file's native rate: rate * fs/44100
        float step = pitch_ * (float)boss_.rate / kRate;
        int need = std::max(1, (int)std::ceil(n * step));
        std::vector<float> raw(need * 2 + 2, 0.0f);
        int got = 0;
        bool fresh = false; // a reopened file that yields nothing stops the stream instead of reopening forever
        while (got < need && boss_.playing) {
            int g = boss_.dec->readStereo(&raw[2 * got], need - got);
            if (g <= 0) {
                // end of file: continue with `next` if this stream continues, else stop (boss_end)
                if (boss_.cont && !fresh) {
                    boss_.cur = boss_.next;
                    boss_.cont = boss_.nextCont;
                    boss_.dec = openBoss(boss_.cur);
                    fresh = true;
                    if (!boss_.dec) boss_.playing = false;
                } else boss_.playing = false;
                continue;
            }
            got += g;
            fresh = false;
        }
        for (int i = 0; i < n; i++) {
            int j = std::min(need - 1, (int)std::floor(i * step + 0.5f));
            mL[i] = mix2(mL[i], raw[2 * j] * tsGain);
            mR[i] = mix2(mR[i], raw[2 * j + 1] * tsGain);
        }
    }

    // menu / background music (looping)
    void mixBackgroundMusic(float* mL, float* mR, int n) {
        bg_.vol = std::min(1.0f, bg_.vol + 0.04f * n / 735.0f);
        // streaming buffer at the file's native rate: step fs/44100, nearest frame
        float step = (float)bg_.dec->sampleRate() / kRate;
        int need = std::max(1, (int)std::ceil(n * step));
        std::vector<float> raw(need * 2 + 2, 0.0f);
        int got = 0;
        while (got < need) {
            int g = bg_.dec->readStereo(&raw[2 * got], need - got);
            if (g <= 0) {
                bg_.dec->seekStart();
                if (bg_.dec->readStereo(&raw[2 * got], 1) <= 0) break;
                bg_.dec->seekStart();
                continue;
            }
            got += g;
        }
        for (int i = 0; i < n; i++) {
            int j = std::min(need - 1, (int)std::floor(i * step + 0.5f));
            if (j >= got) break;
            mL[i] = mix2(mL[i], raw[2 * j] * bg_.vol);
            mR[i] = mix2(mR[i], raw[2 * j + 1] * bg_.vol);
        }
    }

    // UI voices: dry, own pitch only, keep playing while the world is paused
    void mixUiVoices(float* uL, float* uR, int n) {
        for (auto& v : uiVoices_) {
            if (!v.active || !v.s) continue;
            for (int i = 0; i < n; i++) {
                int idx = (int)v.pos;
                if (idx >= v.s->n) {
                    v.active = false;
                    break;
                }
                uL[i] = mix2(uL[i], v.vol * v.s->frames[2 * idx]);
                uR[i] = mix2(uR[i], v.vol * v.s->frames[2 * idx + 1]);
                v.pos += v.pitch;
            }
        }
    }

    void mixChunk(float* out, int n) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<float> sL(n, 0.0f), sR(n, 0.0f), mL(n, 0.0f), mR(n, 0.0f);
        // world audio stops entirely while paused or when the time scale is 0.01 or below
        const bool worldOff = paused_ || pitch_ <= 0.01f;
        const float tsGain = std::min(pitch_, 10.0f); // music volume follows the time scale along with its pitch
        float rateScale = worldOff ? 0.0f : pitch_;
        if (!worldOff) {
            mixVoices(sL.data(), sR.data(), n, rateScale);
            mixLoops(sL.data(), sR.data(), n, rateScale);
        }
        filterWorld(sL.data(), sR.data(), n, worldOff);
        // world SFX volume follows the time scale, applied after the effects. The original replaces the sound setting
        // with the time scale here; multiplying keeps a muted SFX setting muted.
        const float worldGain = worldOff ? 0.0f : soundVol_ * pitch_;
        if (level_.valid && !worldOff) mixLevelMusic(mL.data(), mR.data(), n, tsGain);
        if (boss_.playing && boss_.dec && !worldOff) mixBossMusic(mL.data(), mR.data(), n, tsGain);
        if (bg_.active && bg_.dec) mixBackgroundMusic(mL.data(), mR.data(), n);
        std::vector<float> uL(n, 0.0f), uR(n, 0.0f);
        mixUiVoices(uL.data(), uR.data(), n);
        // final mix
        for (int i = 0; i < n; i++) {
            float l = mix2(mix2(sL[i] * worldGain, uL[i] * soundVol_), mL[i] * musicVol_);
            float r = mix2(mix2(sR[i] * worldGain, uR[i] * soundVol_), mR[i] * musicVol_);
            out[2 * i] = clampf(l, -1, 1);
            out[2 * i + 1] = clampf(r, -1, 1);
        }
    }
};

} // namespace

AudioSystem* createSdlAudio(const Vfs& vfs) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return nullptr;
    auto* a = new SdlAudio(vfs);
    if (!a->open()) {
        delete a;
        return nullptr;
    }
    return a;
}

} // namespace sh
