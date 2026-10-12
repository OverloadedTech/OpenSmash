// Audio system interface used by the game controller. The real implementation (audio_system.cpp) mixes
// Ogg Vorbis music + SFX in software and streams to an SDL3 audio device; NullAudio is used headless.
#pragma once
#include <string>

namespace sh {

class AudioSystem {
public:
    virtual ~AudioSystem() = default;
    virtual int load(const std::string& path) = 0; // returns handle or -1
    virtual void play(int handle, float vol, float pitch, float pan) = 0;
    virtual void playUi(int handle, float vol, float pitch) = 0; // dry UI pool: ignores world pitch and pause
    virtual void playLevelMusic(const std::string& name) = 0; // fresh start of "music/<name>.ogg" (first 2 s skipped)
    virtual void queueNextLevelMusic(const std::string& name) = 0; // next room's track, starts mixing in at 32 s
    virtual void swapLevelMusic() = 0;                             // room transition: queued track becomes current
    virtual bool levelMusicContinues() = 0;                        // false: the room change must start its track fresh
    virtual void stopLevelMusic() = 0;                             // everything: level stream, boss loop, one-shots
    virtual void stopLevelStream() = 0;                            // level-music channel only (boss rooms)
    // boss music: gapless chain of short loops (music/boss_loop0..3.ogg, boss_end.ogg plays once)
    virtual void playBossLoop(const std::string& path) = 0;
    virtual void setNextBossLoop(const std::string& path, bool once) = 0;
    virtual bool bossLoopPlaying() = 0;
    virtual std::string bossLoopName() = 0;
    virtual void stopBossLoop() = 0;
    virtual float levelMusicLocation() = 0; // seconds; 0 without a stream, -1 with music off
    virtual bool musicEnabled() { return levelMusicLocation() > -1.0f; } // when on, the level clock follows the music
    virtual void playBackgroundMusic(const std::string& path) = 0;
    virtual void stopBackgroundMusic() = 0;
    virtual void setReverb(float lowPass, float a, float b, float c, float e0, float e1, float e2, float e3) = 0;
    virtual void setVolumes(float music, float sfx) = 0;
    // world time scale: pitch + volume of level/boss music and world SFX (<= 0.01: frozen)
    virtual void setPitch(float p) = 0;
    virtual void setPaused(bool p) = 0;
    virtual void setLoop(int slot, int handle, float vol, float pan) = 0; // SoundLoop channel 0..2; vol 0 stops it
    virtual void setFlanger(float amount) = 0;                            // level-music flanger, 0 = off
};

class Vfs;
// SDL3-backed software mixer. Returns nullptr if no audio device could be opened.
AudioSystem* createSdlAudio(const Vfs& vfs);

// Silent implementation for headless runs/tests.
class NullAudio : public AudioSystem {
public:
    int load(const std::string&) override { return -1; }
    void play(int, float, float, float) override {}
    void playUi(int, float, float) override {}
    void playLevelMusic(const std::string&) override {}
    void queueNextLevelMusic(const std::string&) override {}
    void swapLevelMusic() override {}
    bool levelMusicContinues() override { return true; }
    void stopLevelMusic() override {}
    void stopLevelStream() override {}
    void playBossLoop(const std::string&) override {}
    void setNextBossLoop(const std::string&, bool) override {}
    bool bossLoopPlaying() override { return false; }
    std::string bossLoopName() override { return ""; }
    void stopBossLoop() override {}
    float levelMusicLocation() override { return -1.0f; }
    void playBackgroundMusic(const std::string&) override {}
    void stopBackgroundMusic() override {}
    void setReverb(float, float, float, float, float, float, float, float) override {}
    void setVolumes(float, float) override {}
    void setPitch(float) override {}
    void setPaused(bool) override {}
    void setLoop(int, int, float, float) override {}
    void setFlanger(float) override {}
};

} // namespace sh
