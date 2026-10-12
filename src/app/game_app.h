// Game controller: state machine (loading -> menu -> play), property bags, commands, scenes (menu/HUD scripts),
// persistence and the host interfaces for the Lua UI and the level simulation.
#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include "audio/audio.h"
#include "game/content.h"
#include "game/level.h"
#include "game/particles.h"
#include "render/game_renderer.h"
#include "render/ui_renderer.h"
#include "ui/ui_script.h"

namespace sh {

struct PointerState {
    bool held = false, pressed = false, released = false;
    float x = 0, y = 0; // window pixels
};

class GameApp : public UiHost, public LevelHost {
public:
    enum class State { Loading, Menu, Play };
    struct Options {
        std::string userDir;    // per-user config directory (progression.xml / config.xml)
        std::string startLevel; // if non-empty: skip the menu and start this selector (e.g. "0")
        int startMode = kModeClassic;
        bool unlockAll = false; // debug: every checkpoint available
        std::string quality = "high";
        bool qualityExplicit = false; // --quality given: wins over the saved setting (and is then saved)
        bool modeExplicit = false;    // --mode given: start in that mode even without --level
        int width = 0, height = 0;    // drawable size at startup (the first menu is laid out for it)
        bool openUrls = false;        // game.url opens the browser (interactive runs only)
        uint32_t randomSeed = 0;      // non-zero: seed the level RNG and rand() with it (0: fixed seeds, deterministic)
    };

    GameApp(Vfs& vfs, GameRenderer& renderer, UiRenderer& ui, const Options& opt);
    ~GameApp() override;
    bool init(std::string* err);
    // One rendered frame: advances the fixed-step simulation, draws everything. `dt` = real frame time (s).
    // `extra`: further pointers that only throw (a second gamepad, more touch fingers: versus / co-op players)
    void frame(double dt, const PointerState& ptr, int pw, int ph, bool singleStep = false,
               const std::vector<PointerState>* extra = nullptr);
    void shutdown();
    bool quitRequested() const { return quit_; }
    void setAudio(AudioSystem* a) {
        audio_ = a;
        applyAudioSettings();
    }
    void applyAudioSettings();

    Level* level() { return level_.get(); }
    State state() const { return state_; }
    void save();
    void backPressed();
    void setFocus(bool focused); // pause the level when the window loses focus
    bool showCrosshair = false;  // draw an aiming crosshair at the pointer (gamepad / keyboard)
    std::vector<std::pair<float, float>> extraCrosshairs; // crosshairs of further gamepads (window pixels)
    int runSerial = 0;                                    // counts level starts (restarts included)

    // --- UiHost ---
    const Vfs& vfs() const override { return vfs_; }
    UiTexInfo texture(const std::string& p) override { return ui_.texture(p); }
    Vec2 viewport() const override { return Vec2((float)pw_, (float)ph_); }
    Mat4 guiToClip() const override { return ui_.clip(); }
    void drawQuad(int t, const Mat4& m, const float uv[4], const float c[4]) override { ui_.drawQuad(t, m, uv, c); }
    void drawMesh(int t, const Mat4& m, const TextMesh& mesh, const float c[4]) override {
        ui_.drawMesh(t, m, mesh, c);
    }
    void drawPie(int t, const Mat4& m, float f, const float uv[4], const float c[4]) override {
        ui_.drawPie(t, m, f, uv, c);
    }
    void drawBorder(int t, const Mat4& m, const float uv[4], const float c[4]) override { ui_.drawBorder(t, m, uv, c); }
    void fullScreenColor(const float c[4], bool half, bool right) override { ui_.fullScreenColor(c, half, right); }
    void scissor(bool on, int x, int y, int w, int h) override { ui_.scissor(on, x, y + viewportY(), w, h); }
    void drawMenuMesh() override;
    int createSound(const std::string& path) override;
    void playSound(int h, float vol, float pitch) override;
    bool touchHeld(int i) const override { return i == 0 && !inputMasked_ && ptr_.held; }
    bool touchPressed(int i) const override { return i == 0 && !inputMasked_ && ptr_.pressed; }
    bool touchReleased(int i) const override { return i == 0 && !inputMasked_ && ptr_.released; }
    Vec2 touchPos(int i) const override { return i == 0 ? Vec2(ptr_.x, ptr_.y) : Vec2(); }
    std::string getProperty(const std::string& key) override;
    void setProperty(const std::string& key, const std::string& value) override;
    void command(const std::string& cmd) override;
    bool highScore(int cp, int& score, int& streak) override;
    int bestDistance() override;
    int latestCheckpoint() override;
    float powerupFraction(const std::string& name) override;
    bool checkpointPos(int i, float& x, float& y) override;
    bool tutorialActive() override { return tutorialId_ != 0; }
    bool tutorialCompleted(int id) override { return tutorialsDone_.count(id) != 0; }
    void markTutorialCompleted(int id) override;
    void startTutorial(int id) override;
    bool remoteBool(const std::string&) override { return false; }
    void multiplierColor(int n, float rgb[3]) override;
    bool worldToGui(const Vec3& p, float& x, float& y) override;
    std::string locale() const override { return locale_; }
    void uiLog(const std::string& s) override;
    void cameraShake(float a) override {
        if (level_) level_->addShake(a);
    }
    void setMode(int m) override { mode_ = validMode(m); }
    static int validMode(int m) { return m >= kModeTraining && m <= kModeCoop ? m : kModeClassic; }
    static bool validQuality(const std::string& q) { return q == "low" || q == "medium" || q == "high"; }

    // --- LevelHost ---
    void hudCommand(const std::string& c) override {
        if (c == "hintpowerup") hintArrowLive_ = true;
        if (hud_ && hud_->loaded()) hud_->handleCommand(c);
    }
    void menuCommand(const std::string& c) override { menu_.handleCommand(c); }
    void sound(const std::string& name, float vol, float pitch, bool positional, const Vec3& pos) override;
    float musicLocation() override;
    bool musicEnabled() override { return audio_ && audio_->musicEnabled(); }
    float menuTransition() override { return menuTransition_; }
    void playLevelMusic(const std::string& name) override;
    void queueNextLevelMusic(const std::string& name) override {
        if (audio_) audio_->queueNextLevelMusic(name);
    }
    void swapLevelMusic() override {
        if (audio_) audio_->swapLevelMusic();
    }
    bool levelMusicContinues() override { return audio_ ? audio_->levelMusicContinues() : true; }
    void playBossLoop(const std::string& p) override {
        if (audio_) audio_->playBossLoop(p);
    }
    void setNextBossLoop(const std::string& p, bool once) override {
        if (audio_) audio_->setNextBossLoop(p, once);
    }
    bool bossLoopPlaying() override { return audio_ && audio_->bossLoopPlaying(); }
    std::string bossLoopName() override { return audio_ ? audio_->bossLoopName() : std::string(); }
    void stopLevelMusicChannel() override {
        if (audio_) audio_->stopLevelStream();
    }
    void setReverb(float lp, float a, float b, float c, float e0, float e1, float e2, float e3) override;
    void playSoundHandle(int h, float vol, float pitch, const Vec3& pos) override;
    void particles(const std::string& effect, const Vec3& pos, const Vec3& vel, const float rgba[4],
                   int count) override;
    void spawnParticle(const std::string& effect, const Vec3& pos, const Vec3& vel, int frame) override;
    void particleBurst(const std::string& effect, const Vec3& pos, float posSpread, const Vec3& vel, float velSpread,
                       const float rgba[4], int count) override {
        effects_.burstCentered(effect, pos, posSpread, vel, velSpread, rgba, count);
    }
    void gameOver(int distance) override;
    void returnToMenu(bool viaCommand) override {
        if (viaCommand) command("game.menu");
        else {
            clearQuickSave();
            enterMenu();
            save();
        }
    }
    void powerupPushed() override;
    void checkpointReached() override;
    void levelEntered(int levelIdx) override {
        levelSel_ = std::to_string(levelIdx);
        props_["game.level"] = levelSel_;
    }
    void loopSound(int slot, const Vec3& pos, float vol) override;
    void quickSave(int room, int score, int balls, int streak) override;
    void clearQuickSave() override;
    void shiftWorldZ(float dz) override { effects_.shift(dz); }
    void releaseBatch(int id) override { renderer_.releaseBatch(id); }
    void setPausedRequest(bool p) override { props_["game.paused"] = p ? "1" : "0"; }

private:
    std::set<int> tutorialsDone_;
    int tutorialId_ = 0;
    struct LoopAcc { // per looping sound: attenuation-weighted position sum and peak volume this frame
        Vec3 sum;
        float w = 0, peak = 0;
    } loopAcc_[3];
    Vfs& vfs_;
    GameRenderer& renderer_;
    UiRenderer& ui_;
    Options opt_;
    Content content_;
    UiGlobals uiGlobals_;
    UiScript menu_;
    std::unique_ptr<UiScript> hud_;
    std::unique_ptr<Level> level_;
    Effects effects_;
    Player saved_; // persistent progression (checkpoints, best distances, stats)
    std::map<std::string, int> lastVariant_;
    std::map<std::string, int> bankLastTick_; // tick of each sound bank's last play
    int sfxTick_ = 0;                         // clock for the bank gates, +1 per tick unless fully paused
    float splashLast_ = -1e9f;                // water/splash bank: level time of the last splash sound
    AudioSystem* audio_ = nullptr;
    State state_ = State::Loading;
    PointerState ptr_;
    bool inputMasked_ = false, quit_ = false;
    int pw_ = 1280, ph_ = 720;        // the game's view (4:3 or wider)
    int winH_ = 720, letterboxY_ = 0; // real window height; top letterbox bar (window pixels, y down)
    double acc_ = 0;
    static constexpr int kLoadDone = 8; // loadStep_ once loading has finished (see loadStep())
    int loadStep_ = 0;
    int gameFrame_ = 0;                            // game.frame for the scripts: 0 while loading, 150 on a quick resume
    float menuAspect_ = 0, menuPendingAspect_ = 0; // aspect the menu was laid out for / latest window aspect
    double menuPendingT_ = 0;                      // how long the latest aspect has been stable
    float menuT_ = 1.0f;                           // linear menu slide progress
    float menuTransition_ = 1.0f;                  // game.menutransition (eased)
    float pauseFade_ = 0;
    std::string levelSel_ = "0";
    std::set<std::string> uiLogged_; // script errors already reported (menu/main.lua errors in draw() every frame)
    // out-of-balls revive: the 10 balls are handed out one by one while paused
    int rewardLeft_ = 0;
    float rewardTimer_ = 0, rewardDelay_ = 0;
    bool rewardActive_ = false;
    bool hintArrowLive_ = false; // the one-shot powerup arrow is showing (until the first pop)
    // a press on a drawn frame without a simulation step reaches the next script tick (above 60 fps)
    bool pendingPress_ = false;
    float pendingPressX_ = 0, pendingPressY_ = 0;
    std::vector<PointerState> extraPtrs_;
    std::string locale_ = "en";
    int mode_ = kModeClassic;
    std::map<std::string, std::string> props_;
    std::map<std::string, int> soundHandles_;
    bool restartRequested_ = false;
    bool interpolate_ = true;
    float hudAspect_ = 0;

    int viewportY() const { return winH_ - ph_ - letterboxY_; } // GL (y up) offset of the view: the bottom bar
    void update();
    void draw();
    void updateMenuTransition(float dt);
    void syncProps();
    void enterMenu();
    void restartLevel(const std::string& name);
    void startLevel();
    void loadHud();
    void stopLevel();
    void loadStep();
    Mat4 gui2D() const;
    Mat4 guiMenu3D() const;
    Mat4 menuProjView() const;
    void playBank(const std::string& prefix, int count, float vol, float pan);
    void loadConfig();
    void saveConfig();
    void reloadMenu();
    void restoreHudState(int hudFlags); // hudFlags: 1 frenzy hint up, 4 pause menu up (read from the old HUD)
    const char* menuMusic() const;
    void loadTutorials();
    void finishTutorial();
    bool updateTutorial(); // true: let this frame's tap through to the level
    bool worldToPixel(const Vec3& p, float& x, float& y) const;
    void refreshPowerupProps();
    void commitLoops(bool active);
    bool bankReady(const std::string& bank, float minInterval);
    void foldProgress();
    void foldStats();
    bool quickLoad(Level::Resume& out);
};

} // namespace sh
