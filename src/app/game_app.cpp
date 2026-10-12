#include "app/game_app.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <SDL3/SDL.h>
extern "C" {
#include <lua.h>
}

namespace sh {

namespace {
std::string word(const std::string& s, int n) {
    std::istringstream in(s);
    std::string w;
    for (int i = 0; i <= n; i++)
        if (!(in >> w)) return "";
    return w;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Scale of the menu's GUI plane: a fit over aspect ratios that keeps the menu camera 51.4 units away (menuProjView)
float menuPlaneScale(float a) {
    return a >= 2.444f ? 0.06262224f * a * a * a - 0.67346728f * a * a + 2.42675905f * a - 1.20009005f
                       : -0.25001880f * std::pow(a, 5) + 3.01955509f * std::pow(a, 4) - 14.11633015f * a * a * a +
                             31.62751007f * a * a - 33.22957992f * a + 14.05222f;
}

Mat4 rotY(float a) {
    Mat4 r;
    r.m[0] = std::cos(a);
    r.m[2] = -std::sin(a);
    r.m[8] = std::sin(a);
    r.m[10] = std::cos(a);
    return r;
}

// view height for a w x h window: narrower than 4:3, the game shows in a 4:3 band (see frame())
int viewHeight(int w, int h) { return w > 0 && (long long)w * 3 < (long long)h * 4 ? std::max(1, w * 3 / 4) : h; }

std::string progPath(const std::string& dir) { return dir + "progression.xml"; }
std::string tutorialPath(const std::string& dir) { return dir + "tutorials.json"; }
// quicksave.xml (the original's quicksave.dat, unencrypted): resume point written 3 ticks into each campaign room.
std::string quickPath(const std::string& dir) { return dir + "quicksave.xml"; }

// sounds behind the camera (larger z) count as 3d+3 away
float effectiveDistance(const Vec3& listener, const Vec3& pos, float d) { return listener.z < pos.z ? 3 * d + 3 : d; }

// Positional sounds pan from the absolute world x with a roughly inverse-square gain.
void positionalGain(const Vec3& listener, const Vec3& pos, float& gain, float& pan) {
    float d = length(pos - listener);
    // at the listener (e.g. the crash sound, emitted at the camera): centred, full volume. The original skips d == 0,
    // but its listener is the display camera, which never coincides exactly with the level camera at that point.
    if (d < 1e-4f) {
        gain = 1;
        pan = 0;
        return;
    }
    pan = clampf(1.5f * pos.x / d, -1, 1);
    float dd = effectiveDistance(listener, pos, d);
    gain = 1.0f / (dd * dd * 0.03f + 1);
}
} // namespace

GameApp::GameApp(Vfs& vfs, GameRenderer& r, UiRenderer& ui, const Options& opt)
    : vfs_(vfs), renderer_(r), ui_(ui), opt_(opt), content_(vfs), menu_(this, &uiGlobals_, "menu"), effects_(vfs) {
    props_ = {{"game.loaded", "0"},
              {"game.frame", "0"},
              {"game.menutransition", "1"},
              {"game.levelpos", "0"},
              {"game.lastdistance", "0"},
              {"game.paused", "0"},
              {"game.premium", "1"},
              {"game.deploy", "1"},
              {"game.purchasing", "0"},
              {"game.premiumprice", ""},
              {"game.premiumorgprice", ""},
              {"game.graphics", opt.quality},
              {"game.uiscale", "1"},
              {"game.platform", "android"},
              {"game.appleArcade", "0"},
              {"game.signedin", "0"},
              {"game.locale", "en"},
              {"game.trymode", "0"},
              {"game.level", "0"},
              {"game.selectedLevel", "0"},
              {"game.showui", "1"},
              {"game.out_of_balls_ad_reward", "10"},
              {"game.new_ballcount_feedback", "-1"},
              {"game.new_ballcount_feedback_delay", "0.5"},
              {"game.hint_mask_x", "0"},
              {"game.hint_mask_y", "0"},
              {"game.challengeType", "distance"},
              {"game.challengeResultAmount", "0"},
              {"audio.musicEnabled", "1"},
              {"audio.soundEnabled", "1"},
              {"player.mode", "1"},
              {"player.startcount", "1"},
              {"player.rated", "0"},
              {"player.adsfront", "0"},
              {"player.adsshown", "0"},
              {"player.adsonlyfree", "0"},
              {"player.adssale", "0"},
              {"player.powerup0", ""},
              {"player.powerup1", ""},
              {"player.powerup2", ""},
              {"player.powerup3", ""},
              {"level.score", "0"},
              {"level.ballcount", "25"},
              {"level.ballcount0", "25"},
              {"level.ballcount1", "25"},
              {"level.streak", "0"},
              {"level.streak0", "0"},
              {"level.streak1", "0"},
              {"level.distance", "0"},
              {"level.progress", "0"},
              {"level.pain", "0"},
              {"level.credits", "0"},
              {"level.moregames", "0"},
              {"level.egg", "0"},
              {"level.lastpickup", "0"},
              {"display.visibleLeft", "0"},
              {"display.visibleTop", "0"},
              {"display.visibleRight", "2048"},
              {"display.visibleBottom", "1536"},
              {"display.width", "1280"},
              {"stats.distance", "0"},
              {"stats.streak", "0"},
              {"stats.peakballs", "0"},
              {"stats.balls", "0"},
              {"stats.ballshit", "0"},
              {"stats.accstreak", "0"},
              {"stats.accballs", "0"},
              {"stats.rooms", "0"},
              {"stats.broken", "0"}};
}

GameApp::~GameApp() = default;

// ---------------------------------------------------------------- init / persistence

bool GameApp::init(std::string* err) {
    if (!content_.ok()) {
        if (err) *err = "game content failed to load";
        return false;
    }
    level_.reset(new Level(content_, this));
    if (opt_.randomSeed) {
        level_->seedRandom(opt_.randomSeed);
        std::srand(opt_.randomSeed);
    }
    saved_ = Player();
    {
        // UI language from the OS preference when the assets ship that localisation
        int n = 0;
        SDL_Locale** loc = SDL_GetPreferredLocales(&n);
        // the first preference that is English or shipped decides (an English first choice stays English)
        for (int i = 0; loc && i < n; i++) {
            std::string lang = loc[i]->language ? loc[i]->language : "";
            if (lang == "en") break;
            if (vfs_.exists("locale/localize_" + lang + "/strings.txt")) {
                locale_ = lang;
                break;
            }
        }
        if (loc) SDL_free(loc);
        if (const char* e = getenv("SH_LOCALE")) locale_ = e;
        props_["game.locale"] = locale_;
        renderer_.setLocale(locale_);
    }
    loadConfig();
    applyAudioSettings(); // the audio system may already be attached (it exists before init)
    loadTutorials();
    mode_ = saved_.mode;
    if (opt_.modeExplicit) mode_ = validMode(opt_.startMode); // this launch only; the saved default stays
    // the menu script reads display.visible*, game.uiscale and the aspect once in init(): give it the real window
    if (opt_.width > 0 && opt_.height > 0) {
        pw_ = opt_.width;
        winH_ = opt_.height;
        ph_ = viewHeight(pw_, winH_);
    }
    syncProps();
    if (opt_.startLevel.empty()) {
        if (!menu_.load("menu/main.lua", {"include/menu_popups.lua"})) {
            if (err) *err = "cannot load menu/main.lua";
            return false;
        }
        menuAspect_ = (float)pw_ / std::max(ph_, 1);
    } else {
        mode_ = validMode(opt_.startMode);
        props_["game.loaded"] = "1";
        loadStep_ = kLoadDone;
        menuT_ = 0;
        menuTransition_ = 0; // menu idle until `game.menu`
        restartLevel(opt_.startLevel);
    }
    return true;
}

void GameApp::loadConfig() {
    if (opt_.userDir.empty()) return;
    std::string s;
    if (readFile(opt_.userDir + "config.xml", s)) {
        if (auto doc = parseXml(s))
            if (const XmlNode* r = doc->child("config"))
                for (auto& a : r->attrs) {
                    if (a.first == "musicEnabled") props_["audio.musicEnabled"] = a.second;
                    else if (a.first == "soundEnabled") props_["audio.soundEnabled"] = a.second;
                    else if (a.first == "graphics" && !opt_.qualityExplicit && validQuality(a.second)) {
                        props_["game.graphics"] = a.second;
                        renderer_.setQuality(a.second);
                    } else if (a.first == "trymode") props_["game.trymode"] = a.second;
                }
    }
    s.clear();
    if (readFile(progPath(opt_.userDir), s)) {
        if (auto doc = parseXml(s))
            if (const XmlNode* r = doc->child("smashhit")) {
                // a corrupt file must not index out of range
                saved_.mode = validMode(atoi(r->attrOr("mode", "1").c_str()));
                if (saved_.mode == kModeZen) saved_.mode = kModeClassic; // the original never starts in zen
                props_["player.startcount"] = std::to_string(atoi(r->attrOr("startcount", "0").c_str()) + 1);
                props_["player.rated"] = r->attrOr("rated", "0");
                props_["player.adsshown"] = r->attrOr("adsshown", "0");
                for (auto& c : r->children) {
                    int m = atoi(c->attrOr("m", "1").c_str());
                    if (m < 0 || m >= kModeCount) continue;
                    if (c->name == "best") saved_.bestDistance[m] = atoi(c->attrOr("v", "0").c_str());
                    else if (c->name == "stats") {
                        Player::StatBank& b = saved_.stats[m];
                        auto ll = [&](const char* k) { return std::max(0LL, atoll(c->attrOr(k, "0").c_str())); };
                        b.balls = ll("balls");
                        b.ballshit = ll("ballshit");
                        b.broken = ll("broken");
                        b.rooms = ll("rooms");
                        b.peakballs = std::max(0, atoi(c->attrOr("peakballs", "0").c_str()));
                        b.streak = std::max(0, atoi(c->attrOr("streak", "0").c_str()));
                        b.accstreak = std::max(0.0, atof(c->attrOr("accstreak", "0").c_str()));
                        b.accballs = std::max(0.0, atof(c->attrOr("accballs", "0").c_str()));
                    } else if (c->name == "ck") {
                        int i = atoi(c->attrOr("i", "0").c_str());
                        if (i >= 0 && i <= kCampaignLevels) {
                            saved_.ckBalls[m][i] = atoi(c->attrOr("balls", "0").c_str());
                            saved_.ckStreak[m][i] = atoi(c->attrOr("streak", "0").c_str());
                        }
                    }
                }
            }
    }
}

void GameApp::saveConfig() {
    if (opt_.userDir.empty()) return;
    FILE* f = fopen((opt_.userDir + "config.xml").c_str(), "wb");
    if (!f) return;
    fprintf(f, "<config musicEnabled=\"%s\" soundEnabled=\"%s\" graphics=\"%s\" trymode=\"%s\"/>\n",
            props_["audio.musicEnabled"].c_str(), props_["audio.soundEnabled"].c_str(), props_["game.graphics"].c_str(),
            props_["game.trymode"].c_str());
    fclose(f);
}

void GameApp::save() {
    if (opt_.userDir.empty()) return;
    saveConfig();
    FILE* f = fopen(progPath(opt_.userDir).c_str(), "wb");
    if (!f) return;
    fprintf(f, "<smashhit mode=\"%d\" startcount=\"%s\" rated=\"%s\" adsshown=\"%s\">\n", saved_.mode,
            props_["player.startcount"].c_str(), props_["player.rated"].c_str(), props_["player.adsshown"].c_str());
    for (int m = 0; m < kModeCount; m++) {
        fprintf(f, "  <best m=\"%d\" v=\"%d\"/>\n", m, saved_.bestDistance[m]);
        for (int i = 0; i <= kCampaignLevels; i++)
            if (saved_.ckBalls[m][i] || saved_.ckStreak[m][i])
                fprintf(f, "  <ck m=\"%d\" i=\"%d\" balls=\"%d\" streak=\"%d\"/>\n", m, i, saved_.ckBalls[m][i],
                        saved_.ckStreak[m][i]);
    }
    // zen and versus banks stay in memory, as in the original
    for (int m : {kModeTraining, kModeClassic, kModeExpert, kModeCoop}) {
        const Player::StatBank& b = saved_.stats[m];
        fprintf(f,
                "  <stats m=\"%d\" balls=\"%lld\" ballshit=\"%lld\" broken=\"%lld\" rooms=\"%lld\" peakballs=\"%d\" "
                "streak=\"%d\" accstreak=\"%.3f\" accballs=\"%.3f\"/>\n",
                m, b.balls, b.ballshit, b.broken, b.rooms, b.peakballs, b.streak, b.accstreak, b.accballs);
    }
    fprintf(f, "</smashhit>\n");
    fclose(f);
}

// each mode has its own menu track
const char* GameApp::menuMusic() const {
    static const char* tracks[kModeCount] = {"music/menu_training.ogg", "music/menu.ogg",    "music/menu_expert.ogg",
                                             "music/menu_zen.ogg",      "music/menu_vs.ogg", "music/menu_vs.ogg"};
    return tracks[validMode(mode_)];
}

void GameApp::applyAudioSettings() {
    if (audio_)
        audio_->setVolumes((float)atof(props_["audio.musicEnabled"].c_str()),
                           (float)atof(props_["audio.soundEnabled"].c_str()));
}

// losing focus pauses the run and opens the pause menu, except in the credits rooms or when something already
// holds the game paused (a tutorial, a popup)
void GameApp::setFocus(bool focused) {
    if (!focused) pendingPress_ = false;
    if (focused || state_ != State::Play || !level_ || !level_->active || level_->ended || level_->creditsLevel) return;
    if (pauseFade_ >= 1.0f || props_["game.paused"] != "0") return;
    props_["game.paused"] = "1";
    hudCommand("resume"); // the HUD script opens its pause menu (ignored while a tutorial is active)
}

void GameApp::backPressed() {
    if (state_ == State::Play) {
        if (hud_ && hud_->loaded()) hud_->handleCommand("back");
    } else if (state_ == State::Menu) menu_.handleCommand("back");
}

void GameApp::shutdown() {
    if (level_ && level_->active) level_->registerLiveBalls();
    foldProgress(); // window closed mid-run: keep the checkpoints reached
    save();
    if (level_) level_->stop();
    level_.reset();
    hud_.reset();
}

// ---------------------------------------------------------------- GUI matrices

Mat4 GameApp::gui2D() const {
    float s = (float)ph_ / kGuiHeight;
    Mat4 p;
    p.m[0] = 2.0f / pw_;
    p.m[5] = -2.0f / ph_;
    p.m[10] = 1;
    p.m[12] = -1;
    p.m[13] = 1;
    return p * Mat4::scale(Vec3(s, s, 1));
}

Mat4 GameApp::menuProjView() const {
    float a = (float)pw_ / ph_;
    float hfov = (a > 2.0f ? 15.0f * a + 30.0f : 60.0f) * kPi / 180.0f;
    float fovY = 2.0f * std::atan(std::tan(hfov * 0.5f) / a);
    // The menu camera looks along -X at the GUI plane (x = 0): GUI x runs along -z, so the corridor scrolls
    // horizontally. menuPlaneScale(a) keeps the camera distance constant at every aspect:
    // D = 38.6 * a / f(a) ~= 51.4
    const float D = 51.4f;
    return Mat4::perspective(fovY, a, 0.2f, 120.0f) * rotY(-kPi / 2) * Mat4::translation(Vec3(-D, -4.0f, 0.0f));
}

Mat4 GameApp::guiMenu3D() const {
    float a = (float)pw_ / ph_;
    Mat4 P = menuProjView();
    float f = menuPlaneScale(a);
    float sx = 0.0284f / f, sy = -0.029f / f;
    float aspectFactor = a / (4.0f / 3.0f);
    Mat4 mv = Mat4::translation(Vec3(0, 4, 0)) * rotY(kPi / 2) * Mat4::scale(Vec3(sx, sy, 1)) *
              Mat4::translation(Vec3(aspectFactor * -1024.0f, -768.0f, 0));
    return P * mv;
}

// ---------------------------------------------------------------- properties

std::string GameApp::getProperty(const std::string& key) {
    auto it = props_.find(key);
    return it == props_.end() ? "" : it->second;
}

void GameApp::setProperty(const std::string& key, const std::string& value) {
    if (key == "game.graphics" && !validQuality(value)) return;
    props_[key] = value;
    if (key == "game.graphics") renderer_.setQuality(value);
    if (key == "audio.musicEnabled" || key == "audio.soundEnabled") applyAudioSettings();
}

void GameApp::syncProps() {
    props_["game.frame"] = std::to_string(gameFrame_);
    props_["game.menutransition"] = std::to_string(menuTransition_);
    props_["game.premium"] = "1";
    props_["game.deploy"] = "1";
    props_["game.locale"] = locale_;
    props_["player.mode"] = std::to_string(mode_);
    float s = (float)ph_ / kGuiHeight;
    props_["display.visibleRight"] = std::to_string(pw_ / s);
    props_["display.width"] = std::to_string(pw_);
    float a = (float)pw_ / ph_;
    float n = clampf((a - 4.0f / 3.0f) / 1.1111111f, 0, 1);
    props_["game.uiscale"] = std::to_string(n * 1.4f + (1 - n));
    // level bag
    if (level_ && level_->active) {
        Level& L = *level_;
        props_["level.ballcount"] = std::to_string(L.player.balls);
        props_["level.ballcount0"] = std::to_string(L.player.ballsP[0]);
        props_["level.ballcount1"] = std::to_string(L.player.ballsP[1]);
        props_["level.streak"] = std::to_string(L.player.streak);
        props_["level.streak0"] = std::to_string(L.player.streakP[0]);
        props_["level.streak1"] = std::to_string(L.player.streakP[1]);
        props_["level.distance"] = std::to_string((int)L.distance);
        props_["level.score"] = std::to_string(L.player.distance);
        props_["level.pain"] = std::to_string(L.pain);
        props_["level.pain_side"] = L.painRight ? "1" : "0"; // which half gets the split-screen pain flash
        float prog = 0;
        if (L.realLevel && L.cur) {
            int li = L.roomDefs[std::min<int>(L.player.roomIdx, (int)L.roomDefs.size() - 1)].levelIdx;
            if (li < kCampaignLevels) prog = clampf((L.distance - li * 1000.0f) / 1000.0f, 0, 1);
        }
        props_["level.progress"] = std::to_string(prog);
        props_["level.credits"] = L.creditsLevel ? "1" : "0"; // credits or more-games room (HUD back button)
        props_["level.moregames"] = L.moregamesLevel ? "1" : "0";
        refreshPowerupProps();
        props_["level.egg"] = L.props["level.egg"].empty() ? "0" : L.props["level.egg"];
    }
    props_["stats.distance"] = std::to_string(saved_.bestDistance[validMode(mode_)]);
    {
        const Player::StatBank& b = saved_.stats[validMode(mode_)];
        props_["stats.streak"] = std::to_string(b.streak);
        props_["stats.peakballs"] = std::to_string(b.peakballs);
        props_["stats.balls"] = std::to_string(b.balls);
        props_["stats.ballshit"] = std::to_string(b.ballshit);
        props_["stats.broken"] = std::to_string(b.broken);
        props_["stats.rooms"] = std::to_string(b.rooms);
        props_["stats.accstreak"] = std::to_string(b.accstreak);
        props_["stats.accballs"] = std::to_string(b.accballs);
    }
}

// ---------------------------------------------------------------- high scores / checkpoints

bool GameApp::highScore(int cp, int& score, int& streak) {
    score = streak = 0;
    if (cp < 1 || cp > kCampaignLevels) return false;
    int m = validMode(mode_);
    score = saved_.ckBalls[m][cp];
    streak = saved_.ckStreak[m][cp];
    if (level_ && level_->active && !level_->creditsLevel) { // the original reads the live Player record
        score = std::max(score, level_->player.ckBalls[m][cp]);
        streak = std::max(streak, level_->player.ckStreak[m][cp]);
    }
    if (opt_.unlockAll && score == 0) score = 25;
    return score > 0;
}
int GameApp::bestDistance() { return saved_.bestDistance[validMode(mode_)]; }
int GameApp::latestCheckpoint() {
    int s, k;
    for (int i = kCampaignLevels; i >= 1; i--)
        if (highScore(i, s, k)) return i;
    return 0;
}
float GameApp::powerupFraction(const std::string& name) {
    int t = powerupFromName(name);
    if (!level_ || !t) return 0;
    return clampf(level_->powerupTimer[t] / kPowerupDuration, 0, 1);
}
void GameApp::multiplierColor(int n, float rgb[3]) {
    // streak multiplier colours, blue -> cyan -> green -> yellow -> red
    static const float cols[10][3] = {{0, 0, 1},    {0, 0.5f, 1}, {0, 1, 1},    {0, 1, 0.5f}, {0, 1, 0},
                                      {0.5f, 1, 0}, {1, 1, 0},    {1, 0.5f, 0}, {1, 0, 0},    {1, 0, 0}};
    for (int i = 0; i < 3; i++) rgb[i] = cols[std::min(std::max(n, 0), 9)][i];
}

// Position (GUI units, in the menu 3D space) of menu-track checkpoint i; consistent with drawMenuMesh().
bool GameApp::checkpointPos(int i, float& x, float& y) {
    float a = (float)pw_ / ph_;
    float f = menuPlaneScale(a);
    float sx = 0.0284f / f, sy = -0.029f / f;
    float cx = a / (4.0f / 3.0f) * 1024.0f;
    float scroll = (float)atof(props_["game.levelpos"].c_str()) * 0.03f;
    float zWorld = -(i * 32.0f + 12.0f) - scroll;
    float yWorld = 3.0f;
    x = cx - zWorld / sx;
    y = 768.0f + (yWorld - 4.0f) / sy;
    return true;
}

// Inverse of Level::shoot's camera ray: level-space point -> window pixel (y down). False if behind the camera.
bool GameApp::worldToPixel(const Vec3& p, float& x, float& y) const {
    x = y = 0;
    if (!level_ || pw_ <= 0 || ph_ <= 0) return false;
    Vec3 d = level_->roll.conj().rotate(p - level_->cam);
    if (d.z >= -1e-4f) return false;
    ViewParams vp;
    vp.width = (float)pw_;
    vp.height = (float)ph_;
    float aspect = vp.width / vp.height, tx = std::tan(vp.fovDeg() * kPi / 360.0f);
    float nx = d.x / -d.z / tx, ny = d.y / -d.z / (tx / aspect);
    x = (nx + 1) * 0.5f * vp.width;
    y = (1 - ny) * 0.5f * vp.height;
    return true;
}

// GUI space = window pixels scaled to the kGuiHeight-unit canvas (gui2D)
bool GameApp::worldToGui(const Vec3& p, float& x, float& y) {
    bool ok = worldToPixel(p, x, y);
    float s = kGuiHeight / (float)std::max(ph_, 1);
    x *= s;
    y *= s;
    return ok;
}

// ---------------------------------------------------------------- tutorials

// [ { "completedTutorials": [ids] } ]; anything malformed = nothing completed
void GameApp::loadTutorials() {
    if (opt_.userDir.empty()) return;
    std::string s;
    if (!readFile(tutorialPath(opt_.userDir), s)) return;
    size_t k = s.find("\"completedTutorials\"");
    size_t a = k == std::string::npos ? k : s.find('[', k), b = a == std::string::npos ? a : s.find(']', a);
    if (b == std::string::npos) {
        fprintf(stderr, "tutorials.json: unreadable, ignored\n");
        return;
    }
    std::stringstream ss(s.substr(a + 1, b - a - 1));
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        int id = atoi(tok.c_str());
        if (id > 0) tutorialsDone_.insert(id);
    }
}

void GameApp::markTutorialCompleted(int id) {
    if (!tutorialsDone_.insert(id).second || opt_.userDir.empty()) return;
    FILE* f = fopen(tutorialPath(opt_.userDir).c_str(), "wb");
    if (!f) return;
    fprintf(f, "[\n\t{\n\t\t\"completedTutorials\": [");
    bool first = true;
    for (int t : tutorialsDone_) {
        fprintf(f, "%s\n\t\t\t%d", first ? "" : ",", t);
        first = false;
    }
    fprintf(f, "\n\t\t]\n\t}\n]");
    fclose(f);
}

void GameApp::startTutorial(int id) {
    tutorialId_ = id;
    props_["game.paused"] = "1";
    hudCommand("hidepausebutton");
    hudCommand("show_tutorial_hint");
}

void GameApp::finishTutorial() {
    tutorialId_ = 0;
    props_["game.paused"] = "0";
    hudCommand("hide_tutorial_hint");
    hudCommand("showpausebutton");
}

bool GameApp::updateTutorial() {
    if (state_ != State::Play || !level_ || !level_->active || !level_->cur || tutorialId_ == 0) return false;
    const char* name = tutorialName(tutorialId_);
    Vec3 target;
    bool hit = false, have = level_->cur->tutorialTarget(name, &target, &hit);
    bool shot = false;
    if (ptr_.pressed && ptr_.y >= 0 && ptr_.y < (float)ph_) { // a press in a letterbox bar is not a tap on the view
        float px, py;
        if (have && worldToPixel(target, px, py) && std::hypot(ptr_.x - px, ptr_.y - py) < 130.0f) {
            level_->adjustedTouch = Vec3(px, py, 0);
            level_->adjustedTouchPending = true;
            finishTutorial();
            shot = true;
        }
    } else if (have && hit) finishTutorial();
    // hint mask around the tutorial target
    if (tutorialId_ != 0 && have) {
        float gx, gy;
        worldToGui(target, gx, gy);
        props_["game.hint_mask_x"] = std::to_string(gx);
        props_["game.hint_mask_y"] = std::to_string(gy);
        hudCommand("showhintmask");
    } else hudCommand("hidehintmask");
    return shot;
}

// ---------------------------------------------------------------- audio glue

int GameApp::createSound(const std::string& path) {
    if (!audio_) return -1;
    auto it = soundHandles_.find(path);
    if (it != soundHandles_.end()) return it->second;
    int h = audio_->load(path);
    soundHandles_[path] = h;
    return h;
}
// menu/HUD scripts
void GameApp::playSound(int h, float vol, float pitch) {
    if (audio_ && h >= 0) audio_->playUi(h, vol, pitch);
}
// level-script mgPlaySound, positioned at the obstacle (doors, bosses, ...)
void GameApp::playSoundHandle(int h, float vol, float pitch, const Vec3& pos) {
    if (!audio_ || h < 0) return;
    float g = 1, pan = 0;
    if (level_) positionalGain(level_->cam, pos, g, pan);
    audio_->play(h, vol * g, pitch, pan);
}
void GameApp::playBank(const std::string& prefix, int count, float vol, float pan) {
    if (!audio_ || count <= 0) return;
    int& last = lastVariant_[prefix];
    int v = count > 1 ? (int)(std::rand() % count) : 0;
    if (count > 1 && v == last) v = (v + 1) % count;
    last = v;
    int h = createSound("snd/" + prefix + std::to_string(v) + ".ogg");
    if (h >= 0) audio_->play(h, vol, 1.0f, pan);
}

void GameApp::loopSound(int slot, const Vec3& pos, float vol) {
    if (slot < 0 || slot > 2 || !level_) return;
    Vec3 l = level_->cam;
    float dd = effectiveDistance(l, pos, length(pos - l));
    float att = vol / (dd * dd * 0.03f + 1);
    LoopAcc& a = loopAcc_[slot];
    a.sum = a.sum + pos * att;
    a.w += att;
    a.peak = std::max(a.peak, att);
}

// Loop sounds pan toward the attenuation-weighted centroid of their emitters and take the loudest one's volume.
void GameApp::commitLoops(bool active) {
    static const char* files[3] = {"snd/laser/loop.ogg", "snd/ball/rollloop.ogg", "snd/object/slideloop.ogg"};
    for (int s = 0; s < 3; s++) {
        LoopAcc& a = loopAcc_[s];
        if (audio_) {
            if (active && a.peak > 0 && a.w > 0) {
                Vec3 c = a.sum / a.w;
                float dist = level_ ? length(c - level_->cam) : 1.0f;
                float pan = clampf(1.5f * c.x / std::max(dist, 1e-4f), -1, 1);
                audio_->setLoop(s, createSound(files[s]), clampf(a.peak, 0, 10), pan);
            } else audio_->setLoop(s, -1, 0, 0); // silent loops stop
        }
        a = LoopAcc();
    }
}

void GameApp::sound(const std::string& name, float vol, float pitch, bool positional, const Vec3& pos) {
    if (!audio_) return;
    float pan = 0;
    if (positional && level_) {
        float g;
        positionalGain(level_->cam, pos, g, pan);
        vol *= g;
    }
    auto single = [&](const char* file, float v) {
        int h = createSound(file);
        if (h >= 0) audio_->play(h, v, pitch, pan);
    };
    // sound banks: variant count and minimum interval between plays (a play inside the interval is dropped)
    struct BankDef {
        int count;
        float minInterval;
    };
    static const std::map<std::string, BankDef> banks = {{"ball/object", {4, 0.1f}},
                                                         {"ball/wall", {4, 0.1f}},
                                                         {"ball/floor", {4, 0.1f}},
                                                         {"ball/ball", {4, 0.1f}},
                                                         {"ball/ricochet", {2, 0.5f}},
                                                         {"object/hit_s", {3, 0.1f}},
                                                         {"object/hit_l", {4, 0.1f}},
                                                         {"object/metal", {2, 0.1f}},
                                                         {"breakable/glass_s", {4, 0.5f}},
                                                         {"breakable/glass_m", {4, 0.5f}},
                                                         {"breakable/glass_l", {4, 0.5f}},
                                                         {"breakable/glass_pane", {4, 0.5f}},
                                                         {"swoosh", {3, 0.0f}},
                                                         {"explosion", {3, 0.5f}},
                                                         {"crash", {1, 0.5f}},
                                                         {"laser/discharge", {1, 0.5f}}};
    auto bank = [&](const std::string& b) { // the gate, then a variant other than the last one
        auto c = banks.find(b);
        if (c == banks.end() || !bankReady(b, c->second.minInterval)) return;
        playBank(b, c->second.count, vol, pan);
    };
    if (name.rfind("bank:", 0) == 0) {
        bank(name.substr(5));
        return;
    }
    if (name.rfind("cleared:", 0) == 0) { // the 12 notes share one 0.05 s gate
        int i = atoi(name.c_str() + 8);
        if (i < 0 || i >= 12 || !bankReady("cleared", 0.05f)) return;
        int h = createSound("snd/cleared" + std::to_string(i) + ".ogg");
        if (h >= 0) audio_->play(h, vol, 1.0f, 0);
    } else if (name == "shoot") bank("swoosh");
    else if (name == "glass_break") bank("breakable/glass_pane");
    else if (name == "rubble") bank("breakable/glass_s");
    else if (name == "hit") bank("crash");
    else if (name == "explosion") bank("explosion");
    // UI sounds in the original, so the fail fade and slow motion do not slow them down
    else if (name == "multiplier_up") audio_->playUi(createSound("snd/multiplier_increase.ogg"), 1.0f, 1.0f);
    else if (name == "streak_lost") audio_->playUi(createSound("snd/multiplier_reset.ogg"), 1.0f, 1.0f);
    else if (name == "out_of_balls_warning") audio_->playUi(createSound("snd/failed.ogg"), vol, 1.0f);
    else if (name == "powerup") single("snd/item.ogg", vol);
    else if (name == "button") single("snd/clickdown0.ogg", vol);
    else if (name.rfind("splash:", 0) == 0) {
        // water splashes: at most one per 0.2 s of game time
        float now = level_ ? level_->time : 0.0f;
        if (now >= splashLast_ && now - splashLast_ < 0.2f) return;
        splashLast_ = now;
        single(("snd/water/splash" + name.substr(7) + ".ogg").c_str(), vol);
    }
}
// A bank stays quiet for minInterval after its last play. Elapsed time is whole ticks times the current level dt,
// which includes the pause fade and slow motion, so the intervals stretch in slow motion. Only a sound that
// actually plays records its tick.
bool GameApp::bankReady(const std::string& bank, float minInterval) {
    float dtNow = state_ == State::Play && level_ && level_->active ? level_->dt : 1.0f / 60;
    auto it = bankLastTick_.find(bank);
    if (it != bankLastTick_.end() && (float)(sfxTick_ - it->second) * dtNow < minInterval) return false;
    bankLastTick_[bank] = sfxTick_;
    return true;
}
float GameApp::musicLocation() { return audio_ ? audio_->levelMusicLocation() : -1.0f; }
void GameApp::playLevelMusic(const std::string& name) {
    if (audio_) audio_->playLevelMusic(name);
}
void GameApp::setReverb(float lp, float a, float b, float c, float e0, float e1, float e2, float e3) {
    if (audio_) audio_->setReverb(lp, a, b, c, e0, e1, e2, e3);
}
void GameApp::particles(const std::string& name, const Vec3& pos, const Vec3& vel, const float* rgba, int count) {
    bool burst = name.rfind("break", 0) == 0 || name.rfind("explosion", 0) == 0 || name == "spark";
    // the nitro trail already carries its own random velocity, and mgSmoke uses exactly the scripted one
    effects_.burst(name, pos, vel, rgba, count, burst ? 0.1f : 0.0f,
                   name.rfind("break", 0) == 0            ? 5.0f
                   : name == "spark"                      ? 4.0f
                   : (name == "trail" || name == "smoke") ? 0.0f
                                                          : 1.0f);
}
void GameApp::spawnParticle(const std::string& name, const Vec3& pos, const Vec3& vel, int frame) {
    static const float white[4] = {1, 1, 1, 1};
    if (ParticleSystem* s = effects_.get(name)) s->spawn(pos, vel, white, frame, effects_.rng);
}

void GameApp::quickSave(int room, int score, int balls, int streak) {
    if (opt_.userDir.empty()) return;
    FILE* f = fopen(quickPath(opt_.userDir).c_str(), "wb");
    if (!f) return;
    fprintf(f,
            "<quicksave version=\"1.5.14\" mode=\"%d\" room=\"%d\" score=\"%d\" balls=\"%d\" streak=\"%d\" "
            "quicksavetimestamp=\"%lld\"/>\n",
            mode_, room, score, balls, streak, (long long)time(nullptr));
    fclose(f);
    foldProgress(); // the resumed run must not lose checkpoints reached before the save
    save();
}

void GameApp::clearQuickSave() {
    if (!opt_.userDir.empty()) remove(quickPath(opt_.userDir).c_str());
}

bool GameApp::quickLoad(Level::Resume& out) {
    if (opt_.userDir.empty()) return false;
    std::string s;
    if (!readFile(quickPath(opt_.userDir), s)) return false;
    auto doc = parseXml(s);
    const XmlNode* r = doc ? doc->child("quicksave") : nullptr;
    if (!r || r->attrOr("version", "") != "1.5.14" || atoi(r->attrOr("mode", "-1").c_str()) != mode_) return false;
    out.room = atoi(r->attrOr("room", "-1").c_str());
    out.balls = atoi(r->attrOr("balls", "25").c_str());
    out.streak = atoi(r->attrOr("streak", "0").c_str());
    return out.room >= 0;
}

void GameApp::gameOver(int distance) {
    clearQuickSave();
    // back to the menu, except in versus where the HUD shows the winner panel. The level then sends "gameover" to the
    // menu script, which compares against its own pre-run best.
    if (level_->player.mode != kModeVersus) {
        enterMenu();
        if (audio_) audio_->playUi(createSound("snd/transition.ogg"), 0.7f, 1.0f);
    }
    int m = validMode(level_->player.mode);
    saved_.bestDistance[m] = std::max(saved_.bestDistance[m], distance);
    foldProgress();
    save();
}

// ---------------------------------------------------------------- state transitions

// Checkpoints and best distances reached in the running level (max-type, safe to fold repeatedly; the original also
// saves checkpoints as soon as they are reached). Additive statistics go through foldStats, which zeroes the per-run
// counters, so folding at every save never counts anything twice.
void GameApp::foldProgress() {
    if (!level_ || !level_->active || level_->creditsLevel) return; // the credits / more-games rooms are not runs
    const Player& p = level_->player;
    int cm = validMode(p.mode); // the running distance counts as soon as the run ends, however it ends
    saved_.bestDistance[cm] = std::max(saved_.bestDistance[cm], p.distance);
    foldStats();
    for (int m = 0; m < kModeCount; m++) {
        saved_.bestDistance[m] = std::max(saved_.bestDistance[m], p.bestDistance[m]);
        for (int i = 0; i <= kCampaignLevels; i++) {
            saved_.ckBalls[m][i] = std::max(saved_.ckBalls[m][i], p.ckBalls[m][i]);
            saved_.ckStreak[m][i] = std::max(saved_.ckStreak[m][i], p.ckStreak[m][i]);
        }
    }
}

// The running level's statistics into the mode's lifetime bank. Additive counters are zeroed after adding, so this
// can run at every save (quick-save, game over, menu, window close) without counting anything twice.
void GameApp::foldStats() {
    if (!level_ || !level_->active || level_->creditsLevel) return;
    Player& p = level_->player;
    Player::StatBank& b = saved_.stats[validMode(p.mode)];
    b.balls += p.statBallsShot;
    b.ballshit += p.statBallsHit;
    b.broken += p.statBroken;
    b.rooms += p.statRooms;
    b.accstreak += p.statAccStreak;
    b.accballs += p.statAccBalls;
    b.streak = std::max(b.streak, p.statMaxStreak);
    b.peakballs = std::max(b.peakballs, p.statPeakBalls);
    p.statBallsShot = p.statBallsHit = p.statBroken = p.statRooms = 0;
    p.statAccStreak = p.statAccBalls = 0;
}

void GameApp::stopLevel() {
    if (!level_) return;
    tutorialId_ = 0;
    if (level_->active) {
        // fold run statistics into the persistent player record
        level_->registerLiveBalls();
        foldProgress();
        level_->stop();
    }
    hud_.reset();
    effects_.clear();
    if (audio_) audio_->stopLevelMusic();
    rewardActive_ = false;
    rewardLeft_ = 0;
    rewardTimer_ = 0;
    props_["game.new_ballcount_feedback"] = "-1";
    hintArrowLive_ = false;
}

// A HUD reloaded mid-run (window resized to another aspect) gets back the popups the level is waiting on: the
// out-of-balls choice, the versus winner, the pause menu, the tutorial hint.
void GameApp::restoreHudState(int hudFlags) {
    if (state_ != State::Play || !hud_ || !hud_->loaded() || !level_ || !level_->active) return;
    if (level_->gameOverState == kGameOverOffer && !rewardActive_) hud_->handleCommand("showoutofballs");
    else if (level_->ended && level_->player.mode == kModeVersus) hud_->handleCommand("showwinner");
    else if (props_["game.paused"] != "0" && tutorialId_ == 0 && !rewardActive_) {
        if (hudFlags & 1) hud_->handleCommand("hintfrenzy");                         // still waiting for its tap
        if (!(hudFlags & 1) || (hudFlags & 4)) hud_->handleCommand("showpausemenu"); // also when opened over the hint
    }
    if (hintArrowLive_) hud_->handleCommand("hintpowerup");
    if (tutorialId_) {
        hud_->handleCommand("hidepausebutton");
        hud_->handleCommand("show_tutorial_hint");
    }
    if (!level_->player.powerups.empty()) { // stored powerups
        refreshPowerupProps();
        hud_->handleCommand("pushPowerup");
    }
}

// Script errors go to stderr once each: the shipped menu script compares an unset global in draw() until the first
// popup opens (it does that in the original too), which would otherwise print every frame.
// SH_UI_LOG_ALL=1 prints every occurrence.
void GameApp::uiLog(const std::string& s) {
    static const bool all = getenv("SH_UI_LOG_ALL") != nullptr;
    if (!all && (uiLogged_.size() >= 1000 || !uiLogged_.insert(s).second)) return;
    fprintf(stderr, "[ui] %s\n", s.c_str());
}

// player.powerup0..3, top of the stack first. The HUD reads them in pushPowerup/popPowerup.
void GameApp::refreshPowerupProps() {
    if (!level_) return;
    const std::vector<int>& st = level_->player.powerups;
    for (int i = 0; i < 4; i++)
        props_["player.powerup" + std::to_string(i)] = powerupName(i < (int)st.size() ? st[st.size() - 1 - i] : 0);
}

void GameApp::powerupPushed() {
    refreshPowerupProps();
    hudCommand("pushPowerup");
}

// saved before the HUD gets "checkpoint": its 'saved' badge compares the run's balls with the stored record
void GameApp::checkpointReached() {
    foldProgress();
    save();
}

// The HUD script caches its layout from display.visible*, so it must see the real window size at load time.
void GameApp::loadHud() {
    syncProps();
    hudAspect_ = (float)pw_ / std::max(ph_, 1);
    hud_.reset(new UiScript(this, &uiGlobals_, "hud"));
    hud_->load("hud/main.lua", {"include/hud_popups.lua"});
}

void GameApp::startLevel() {
    stopLevel();
    if (audio_) audio_->stopBackgroundMusic(); // the level track waits until the menu has slid away
    level_->player = saved_;
    // run statistics count this run only; foldStats adds them to the mode's bank
    level_->player.clearRunStats();
    level_->player.mode = mode_;
    level_->props["game.graphics"] = props_["game.graphics"];
    props_["game.level"] = levelSel_;
    const std::string sel = levelSel_; // levelEntered() rewrites levelSel_ from inside Level::start
    if (!level_->start(sel, mode_)) {
        fprintf(stderr, "cannot start level %s\n", sel.c_str());
        state_ = State::Menu;
        return;
    }
    loadHud();
    props_["game.paused"] = "0";
    pauseFade_ = 0;
    if (audio_) audio_->setPaused(false);
    state_ = State::Play;
    runSerial++;
    syncProps();
    hud_->tick(1.0f / 60, 1.0f / 60); // scripts expect frame() to run before the first draw()
}

void GameApp::restartLevel(const std::string& name) {
    std::string sel = name;
    if (name.find("credits") == std::string::npos && name.find("moregames") == std::string::npos) {
        if (mode_ == kModeZen) sel = "level:zen";
        else if (mode_ == kModeVersus) sel = "level:versus";
        else if (mode_ == kModeCoop) sel = "level:coop";
    }
    levelSel_ = sel;
    startLevel();
    // after startLevel: stopLevel() clears the world voices
    if (state_ == State::Play && audio_) audio_->play(createSound("snd/transition.ogg"), 0.7f, 1.0f, 0);
}

// The menu script laid out again for a new window aspect (it reads the display size only in init/load). The new
// instance takes over the old one's boot, scroll and score state, so a resize during the boot (loading screen, menu
// music cue, scroll to the latest checkpoint) or after a run changes nothing but the layout.
void GameApp::reloadMenu() {
    static const char* const kKeep[] = {"loadingFrame",     "scrollFrame",       "currentHighscore",
                                        "displayHighscore", "highscoreProgress", "displayRank",
                                        "targetRank",       "rankFrame",         "__relayoutX"};
    std::vector<std::pair<const char*, double>> keep;
    std::vector<UiScript::Delayed> pending = menu_.delayed; // startMenuScroll() / onMenuScrollDone() / popups
    float scrollLeft = -1;
    for (const auto& d : pending)
        if (d.code == "onMenuScrollDone()") scrollLeft = d.remaining;
    if (menu_.lua()) {
        std::string e;
        menu_.lua()->run("__relayoutX = levelCanvas and (mgGetPos(levelCanvas)) or nil", "relayout", &e);
        lua_State* L = menu_.lua()->L();
        for (const char* k : kKeep) {
            lua_getglobal(L, k);
            if (lua_isnumber(L, -1)) keep.emplace_back(k, lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
    }
    syncProps();
    menu_.load("menu/main.lua", {"include/menu_popups.lua"});
    menu_.handleCommand("load");
    // after the loading screen the logo is only positioned by drawLoading(): put it back where the boot left it
    if (gameFrame_ > 100 && menu_.lua()) {
        std::string e;
        menu_.lua()->run("if loadingLogo then mgSetPos(loadingLogo, 4, -480) end", "relayout", &e);
    }
    menu_.handleCommand("activate");
    if (menu_.lua()) {
        lua_State* L = menu_.lua()->L();
        bool haveX = false;
        for (auto& kv : keep) {
            lua_pushnumber(L, kv.second);
            lua_setglobal(L, kv.first);
            haveX |= !strcmp(kv.first, "__relayoutX");
        }
        std::string e;
        if (scrollLeft > 0) // the boot scroll was under way: finish it from where it was
            menu_.lua()->run(
                "if __relayoutX then mgSetPos(levelCanvas, __relayoutX, 0) end mgSetCanvasEnabled(levelCanvas, false) "
                "mgSetPos(levelCanvas, distanceToCanvasPos(1000 * mgLatestAvailableCheckpoint()), 0, \"easeout\", " +
                    std::to_string(scrollLeft) + ")",
                "relayout", &e);
        else if (haveX) // the level strip stays where it was (canvas x is distance-based, not aspect-based)
            menu_.lua()->run("mgSetPos(levelCanvas, __relayoutX, 0)", "relayout", &e);
        menu_.lua()->run("__relayoutX = nil", "relayout", &e);
    }
    menu_.delayed.insert(menu_.delayed.end(), pending.begin(), pending.end());
    menuAspect_ = (float)pw_ / std::max(ph_, 1);
}

void GameApp::enterMenu() {
    if (state_ == State::Menu) return;
    state_ = State::Menu;
    // the menu script lives for the whole session (the original reloads it only on a debug key). "activate" scrolls
    // to where the run ended and keeps the pre-run best for the count-up.
    if (loadStep_ >= kLoadDone) {
        // never set up (a --level start), or the window aspect changed during the run: lay it out again before the
        // game-over panel is built in it; "activate" again because the reload restores the pre-run strip position
        if (!menu_.loaded() || std::fabs((float)pw_ / std::max(ph_, 1) - menuAspect_) > 0.01f) reloadMenu();
        menu_.handleCommand("activate");
    }
    props_["game.paused"] = "0";
    if (audio_) {
        // world channels off, the mode's menu track on
        audio_->setPaused(false);
        audio_->setPitch(1.0f);
        audio_->stopLevelStream();
        audio_->stopBossLoop();
        audio_->playBackgroundMusic(menuMusic());
    }
}

// one call per game.load from the menu's loading screen: the 7th loads the menu, the 8th ends loading
void GameApp::loadStep() {
    const int step = ++loadStep_;
    if (step == kLoadDone - 1) menu_.handleCommand("load");
    if (step >= kLoadDone) {
        props_["game.loaded"] = "1";
        state_ = State::Menu;
        // continue an interrupted campaign run, paused
        Level::Resume r;
        if (mode_ != kModeZen && mode_ != kModeVersus && mode_ != kModeCoop && quickLoad(r)) {
            clearQuickSave();
            level_->resume = r;
            gameFrame_ = 150; // skips the menu's loading overlay
            levelSel_ = "quickload";
            startLevel();
            if (state_ == State::Play) { // levelSel_ is now the resumed room's level (levelEntered)
                props_["game.paused"] = "1";
                pauseFade_ = 1.0f;
                if (hud_ && hud_->loaded()) hud_->handleCommand("resume");
                return;
            }
        }
        menu_.handleCommand("startmenuscroll");
    }
}

// ---------------------------------------------------------------- commands

void GameApp::command(const std::string& cmdFull) {
    std::string cmd = word(cmdFull, 0), a1 = word(cmdFull, 1);
    if (cmd == "level.start") restartLevel(a1);
    else if (cmd == "level.restart") restartRequested_ = true;
    else if (cmd == "game.gameover_restart") restartLevel(a1.empty() ? "0" : a1);
    else if (cmd == "game.menu") {
        clearQuickSave();
        enterMenu();
        if (audio_) audio_->playUi(createSound("snd/transition.ogg"), 0.7f, 1.0f);
        foldProgress(); // checkpoints, best distance and statistics of the abandoned run
        save();
    } else if (cmd == "game.quit") quit_ = true;
    else if (cmd == "game.resume") {
        props_["game.paused"] = "0";
        pauseFade_ = 0;
        if (audio_) audio_->setPaused(false);
    } else if (cmd == "game.mode") {
        int m = a1 == "training" ? kModeTraining
                : a1 == "zen"    ? kModeZen
                : a1 == "expert" ? kModeExpert
                : a1 == "versus" ? kModeVersus
                : a1 == "coop"   ? kModeCoop
                                 : kModeClassic;
        int old = mode_;
        mode_ = m;
        saved_.mode = m;
        if (audio_ && state_ == State::Menu) audio_->playBackgroundMusic(menuMusic()); // no-op if already playing
        if (old != m) {
            menu_.handleCommand("newmode");
            props_["game.lastdistance"] = "0";
        }
        props_["player.mode"] = std::to_string(m);
        save();
    } else if (cmd == "game.togglegraphics") {
        std::string g = props_["game.graphics"];
        setProperty("game.graphics", g == "low" ? "medium" : g == "medium" ? "high" : "low");
    } else if (cmd == "audio.togglemusic" || cmd == "audio.togglesound") {
        bool music = cmd == "audio.togglemusic";
        std::string key = music ? "audio.musicEnabled" : "audio.soundEnabled";
        std::string v = props_[key];
        bool cycle = v == "0.3" || v == "0.7" || v == "1";
        setProperty(key, v == "0.3" ? "0.7" : v == "0.7" ? "1" : v == "1" ? "0" : "0.3"); // volumes first
        // switching music back on ("0" or anything unexpected -> "0.3") restarts the mode's menu track from the
        // beginning, since it was released while music was off
        if (music && !cycle && audio_) audio_->playBackgroundMusic(menuMusic());
    } else if (cmd == "game.saveConfig") saveConfig();
    else if (cmd == "player.rate") {
        props_["player.rated"] = "1";
        save();
    } else if (cmd == "player.adsshown") {
        props_["player.adsshown"] = "1";
        save();
    } else if (cmd == "player.activatepowerup") {
        // refresh while powerup0 is still the one being used, let the HUD animate it out (and hide the button after
        // the last one), then pop
        if (level_ && level_->active && !level_->player.powerups.empty()) {
            refreshPowerupProps();
            hintArrowLive_ = false; // popPowerup() fades the hint arrow out
            if (hud_ && hud_->loaded()) hud_->handleCommand("popPowerup");
            int t = level_->player.popPowerup();
            if (t) level_->activatePowerup(t);
        }
    } else if (cmd == "game.showOutOfBallsAd") {
        // out-of-balls revive: stay paused while update() counts the balls in
        if (level_ && level_->active && level_->gameOverState == kGameOverOffer && !rewardActive_) {
            if (hud_ && hud_->loaded()) hud_->handleCommand("hideoutofballs");
            rewardActive_ = true;
            rewardLeft_ = 10;
            rewardTimer_ = rewardDelay_ = 0.3f;
            props_["game.new_ballcount_feedback"] = "10";
            props_["game.paused"] = "1";
        }
    } else if (cmd == "game.declineOutOfBallsAd") {
        if (level_) level_->onOutOfBallsSkip();
        props_["game.paused"] = "0";
    } else if (cmd == "game.showAdForCheckpoint" || cmd == "game.rewind") restartLevel("0");
    else if (cmd == "game.showupgrade") menu_.handleCommand("showupgradedelayed");
    else if (cmd == "game.load") loadStep();
    else if (cmd == "audio.playBackgroundMusic") {
        if (audio_) audio_->playBackgroundMusic(a1);
    } else if (cmd == "audio.stopBackgroundMusic") {
        if (audio_) audio_->stopBackgroundMusic();
    } else if (cmd == "hud.reload") {
        if (hud_) hud_->load("hud/main.lua", {"include/hud_popups.lua"});
    } else if (cmd == "game.url") {
        // open the browser. Web links only, since the scripts come from the asset files, and never from automated runs.
        if (opt_.openUrls && (a1.rfind("http://", 0) == 0 || a1.rfind("https://", 0) == 0) && !SDL_OpenURL(a1.c_str()))
            fprintf(stderr, "[game] cannot open %s: %s\n", a1.c_str(), SDL_GetError());
    }
    // anything else (game.gameover_close, ads, analytics, IAP, leaderboards, popup tracking) does nothing on desktop
}

// ---------------------------------------------------------------- per frame

void GameApp::updateMenuTransition(float dt) {
    bool inMenu = state_ == State::Menu || state_ == State::Loading;
    menuT_ = clampf(menuT_ + (inMenu ? 0.5f : -0.5f) * dt, 0, 1);
    float t = menuT_;
    menuTransition_ = inMenu ? 1 - (1 - std::cos(kPi * (1 - t) * (1 - t))) / 2 : (1 - std::cos(kPi * t * t)) / 2;
}

void GameApp::update() {
    const float dt = 1.0f / 60;
    if (state_ != State::Loading) gameFrame_++; // game.frame does not advance while loading
    syncProps();
    if (state_ == State::Loading || menuTransition_ > 0.05f) menu_.tick(dt, dt);
    // the HUD ticks whenever menutransition < 0.95, in any state, so it keeps fading out and runs its exit tweens after
    // the run is left (stopLevel() unloads it at mt > 0.9). Outside Play it gets no input, so a tap during the fade
    // cannot pause the menu or restart the level.
    if (hud_ && hud_->loaded() && menuTransition_ < 0.95f) {
        inputMasked_ = state_ != State::Play;
        hud_->tick(dt, dt);
        inputMasked_ = false;
    }
    updateMenuTransition(dt);
    if (restartRequested_) {
        // level.restart from the pause menu restarts the current level without the transition sound. The pause fade is
        // kept and decays at 0.05/tick, so the new run spins up from the paused state.
        restartRequested_ = false;
        const float fade = pauseFade_;
        startLevel();
        if (state_ == State::Play) pauseFade_ = fade;
    }
    // revive: one ball per delay (0.3 s, shrinking by x0.91 each time), then resume
    if (rewardActive_ && (rewardTimer_ -= dt) < 0) {
        if (state_ == State::Play && level_ && level_->active && level_->gameOverState == kGameOverOffer) {
            if (rewardLeft_ > 0) {
                if (audio_) audio_->playUi(createSound("snd/multiplier_increase.ogg"), 1.0f, 1.0f);
                level_->player.balls++;
                rewardLeft_--;
                props_["game.new_ballcount_feedback"] = std::to_string(rewardLeft_); // the HUD's count-up
                props_["game.new_ballcount_feedback_delay"] = std::to_string(rewardDelay_);
                rewardDelay_ *= 0.91f;
                rewardTimer_ = rewardDelay_;
            } else {
                rewardActive_ = false;
                props_["game.new_ballcount_feedback"] = "-1";
                props_["game.paused"] = "0";
                level_->onOutOfBallsReward(10);
            }
        } else rewardActive_ = false;
    }
    bool paused = props_["game.paused"] != "0";
    pauseFade_ = paused ? std::min(1.0f, pauseFade_ + 0.05f) : std::max(0.0f, pauseFade_ - 0.05f);
    if (state_ == State::Play && level_ && level_->active && pauseFade_ != 1.0f) {
        level_->pauseFade = pauseFade_;
        level_->update(dt);
        commitLoops(true);
        effects_.update(level_->dt);
    } else commitLoops(false);
    if (pauseFade_ != 1.0f) sfxTick_++;
    // world time scale ts = (1 - failFade) * (1 - pauseFade) * slow motion sets pitch and volume of the level/boss
    // music and world sounds (frozen at ts <= 0.01). Only while playing, the menu must not inherit it.
    if (audio_)
        audio_->setPitch(state_ == State::Play && level_ && level_->active
                             ? (1.0f - level_->outOfBalls) * (1.0f - pauseFade_) * level_->timeScale
                             : 1.0f);
    if (audio_) {
        audio_->setPaused(pauseFade_ >= 1.0f);
        // level-music flanger: A = sin(pi * max powerup timer / 7), off when no powerup runs
        float x = 0;
        if (state_ == State::Play && level_ && level_->active)
            for (float t : level_->powerupTimer) x = std::max(x, t / kPowerupDuration);
        audio_->setFlanger(x > 0 ? std::sin(kPi * std::min(x, 1.0f)) : 0.0f);
    }
    if (state_ == State::Menu && level_ && level_->active && menuTransition_ > 0.9f) stopLevel();
}

void GameApp::drawMenuMesh() {
    // 3D menu corridor behind the level buttons (an approximation of the original)
    float scroll = (float)atof(props_["game.levelpos"].c_str()) * 0.03f;
    float t = clampf((menuTransition_ - 0.5f) * 2.0f, 0, 1);
    unsigned reached = 0; // tile i shows its level once checkpoint i has a high score (same source as mgGetHighScore)
    for (int i = 1; i <= kCampaignLevels; i++) {
        int s, k;
        if (highScore(i, s, k)) reached |= 1u << i;
    }
    renderer_.drawMenuStrips(menuProjView(), scroll, validMode(mode_), t, reached);
}

void GameApp::draw() {
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::Disable(GL_SCISSOR_TEST);
    gl::DepthMask(GL_TRUE);
    gl::ClearColor(0, 0, 0, 0);
    gl::Viewport(0, 0, pw_, winH_);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); // also the letterbox bars
    gl::Viewport(0, viewportY(), pw_, ph_);
    bool levelDraw = level_ && level_->active && level_->cur;
    renderer_.interp = interpolate_ ? clampf((float)(acc_ * 60.0), 0, 1) : 1.0f;
    if (levelDraw) {
        // particles as camera-aligned sprites
        renderer_.cameraZ = level_->cam.z;
        renderer_.playerMode = level_->player.mode; // the particle tint below runs before drawLevel
        std::vector<ParticleQuad> pq;
        effects_.collect(pq);
        for (auto& q : pq) {
            Quat rot = level_->roll;
            float hw = q.half, hh = q.half;
            // streaks (vel.xy == 0) lie in the world y-z plane without roll and are camSpeed*half long, so they run
            // along the view axis and vanish when the camera stops
            if (q.streak) {
                rot = Quat::axisAngle(Vec3(0, 1, 0), kPi / 2);
                hw = level_->camSpeed * q.half;
                hh = q.half * 0.3f;
            } else rot = level_->roll * Quat::axisAngle(Vec3(0, 0, 1), q.angle);
            renderer_.addUvSprite(renderer_.extraSprites, q.pos, rot, hw, hh, q.rgba, q.uv, q.blend);
        }
        renderer_.hudOn = hud_ && hud_->loaded(); // the best-distance marker shows until the level stops
        renderer_.bestDistance = bestDistance();
        renderer_.drawLevel(*level_, pw_, ph_);
    }
    ui_.beginFrame(pw_, ph_);
    bool maskInput = state_ == State::Play;
    // menu scene (3D menu layer then GUI layer)
    if (state_ == State::Loading || menuTransition_ > 0.05f) {
        inputMasked_ = maskInput;
        gl::Clear(GL_DEPTH_BUFFER_BIT);
        ui_.setClip(guiMenu3D(), true);
        menu_.drawWorld();
        gl::Clear(GL_DEPTH_BUFFER_BIT);
        ui_.setClip(gui2D(), false);
        menu_.draw();
        inputMasked_ = false;
    }
    ui_.setClip(gui2D(), false);
    if (hud_ && hud_->loaded() && menuTransition_ < 0.95f) { // same condition as its tick, in any state
        inputMasked_ = state_ != State::Play;
        hud_->draw();
        inputMasked_ = false;
    }
    gl::Disable(GL_DEPTH_TEST);
    if (showCrosshair) ui_.drawCrosshair(ptr_.x, ptr_.y, ph_ * 0.03f);
    for (auto& c : extraCrosshairs) ui_.drawCrosshair(c.first, c.second - (float)letterboxY_, ph_ * 0.03f);
    uiEndFrame(*this, uiGlobals_);
}

void GameApp::frame(double dt, const PointerState& ptr, int pw, int ph, bool singleStep,
                    const std::vector<PointerState>* extra) {
    // windows narrower than 4:3 (portrait, square) show the game in a centred 4:3 band: the original's menu/HUD layout
    // and camera fits were made for 4:3 and wider
    winH_ = ph;
    ph = viewHeight(pw, ph);
    letterboxY_ = (winH_ - ph) / 2; // top bar in window pixels (pointer space)
    pw_ = pw;
    ph_ = ph;
    ptr_ = ptr;
    ptr_.y -= (float)letterboxY_;
    extraPtrs_.clear();
    if (extra)
        for (PointerState p : *extra) {
            p.y -= (float)letterboxY_;
            extraPtrs_.push_back(p);
        }
    if (hud_ && std::fabs((float)pw / std::max(ph, 1) - hudAspect_) > 0.01f) { // new aspect: HUD laid out again
        int flags = 0; // what the old HUD shows that the level does not know about
        if (hud_->loaded() && hud_->lua()) {
            std::string e;
            if (hud_->lua()->run("__rs = (hintFrenzy and mgGetAlpha(hintFrenzy) > 0 and 1 or 0) + (pauseMenuUi and "
                                 "mgGetAlpha(pauseMenuUi) > 0 and 4 or 0)",
                                 "relayout", &e)) {
                lua_State* L = hud_->lua()->L();
                lua_getglobal(L, "__rs");
                flags = (int)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
        loadHud();
        hud_->tick(1.0f / 60, 1.0f / 60); // its frame() must run before the first draw (as in startLevel)
        restoreHudState(flags);
    }
    { // menu: lay it out again once a new aspect has been stable for 0.25 s and no popup is open (resize, F11)
        float aspect = (float)pw / std::max(ph, 1);
        if (std::fabs(aspect - menuPendingAspect_) > 0.001f) {
            menuPendingAspect_ = aspect;
            menuPendingT_ = 0;
        } else menuPendingT_ += dt;
        if (state_ == State::Menu && loadStep_ >= kLoadDone && menuTransition_ > 0.95f &&
            std::fabs(aspect - menuAspect_) > 0.01f && menuPendingT_ >= 0.25 && uiGlobals_.modal.empty())
            reloadMenu();
    }
    acc_ += std::min(dt, 0.1);
    int steps = 0;
    // while loading, exactly one menu tick per drawn frame. menu/main.lua counts loadingFrame per draw and sends
    // game.load from its tick at 10, 20, ... 80. Above 60 fps fixed-step ticks would skip those values and the game
    // would never leave the loading screen.
    const bool oneTick = singleStep || state_ == State::Loading;
    interpolate_ = !oneTick;
    const bool fullyPaused = pauseFade_ >= 1.0f; // level input is off only once the pause fade is complete
    // above 60 fps some drawn frames run no step: a press on such a frame is handed to the next script tick (canvas
    // drags, scripts reading the touch), at the position where it happened
    const bool rawPressed = ptr_.pressed;
    const float rawX = ptr_.x, rawY = ptr_.y;
    if (!rawPressed && pendingPress_) {
        ptr_.pressed = true;
        ptr_.x = pendingPressX_;
        ptr_.y = pendingPressY_;
    }
    if (oneTick) {
        update();
        steps = 1;
        acc_ = 0;
    } else
        while (acc_ >= 1.0 / 60 && steps < 2) {
            update();
            acc_ -= 1.0 / 60;
            steps++;
        }
    if (steps > 0) pendingPress_ = false;
    else if (rawPressed) {
        pendingPress_ = true;
        pendingPressX_ = rawX;
        pendingPressY_ = rawY;
    }
    ptr_.pressed = rawPressed;
    ptr_.x = rawX;
    ptr_.y = rawY; // the UI and the level see this frame's own edge
    if (steps == 0) syncProps();
    if (acc_ > 0.0334) acc_ = 0.0166;
    draw();
    const bool tutorialShot = updateTutorial();
    // level input is read after the UI so widgets can take the touch. The tap that ends a tutorial goes through
    // although the game is still paused.
    if (state_ == State::Play && level_ && level_->active && (tutorialShot || !fullyPaused) &&
        uiGlobals_.pressedId == -1) {
        std::vector<InputTap> taps;
        auto add = [&](const PointerState& p) {
            if (p.y < 0 || p.y >= (float)ph_) return; // a press in a letterbox bar throws nothing
            InputTap tap;
            tap.down = p.held;
            tap.pressed = p.pressed;
            tap.released = p.released;
            tap.x = p.x;
            tap.y = p.y;
            taps.push_back(tap);
        };
        add(ptr_);
        if (!tutorialShot)
            for (auto& p : extraPtrs_) add(p); // each touch / pad is one tap (split-screen players)
        ViewParams vp;
        vp.width = (float)pw_;
        vp.height = (float)ph_;
        if (!taps.empty()) level_->handleInput(taps.data(), (int)taps.size(), vp);
    }
}

} // namespace sh
