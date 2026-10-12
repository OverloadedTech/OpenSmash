// Draws a Level (rooms, dynamic shapes, glass, sprites, shadows) with OpenGL 3.3 core.
#pragma once
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "game/level.h"
#include "render/gfx.h"

namespace sh {

// which passes draw a sprite: the normal pass, the floor reflection, or both
enum SpriteMode { kSpriteDirect, kSpriteDirectAndMirror, kSpriteMirrorOnly };

struct SpriteQuad {
    Vec3 c[4];
    float uv[4][2];
    uint32_t color;
    float blend;
    SpriteMode mode;
    bool doors;
    float sortZ;
};

class GameRenderer {
public:
    GameRenderer();
    ~GameRenderer();
    bool init(const Vfs& vfs, std::string* err);
    void shutdown();
    // Draws the 3D scene into the currently bound framebuffer area (0,0,w,h).
    void drawLevel(Level& level, int w, int h);
    void releaseBatch(int gpuId);
    // 3D menu corridor, approximating the original: strips of segments/menu/N.mesh along -z, scrolled by `scroll` world
    // units. Bit i of `reached` = checkpoint i reached in this mode (its tile shows the level's mesh, else
    // hidden.mesh).
    void drawMenuStrips(const Mat4& clip, float scroll, int mode, float alpha, unsigned reached);
    void setQuality(const std::string& q) { quality_ = q; }
    void setLocale(const std::string& lang) { locale_ = lang; } // before textures with localized variants load
    bool hudOn = false;                                         // gates the best-distance marker
    int bestDistance = 0;                                       // player's best for the current mode
    float interp = 1.0f; // 0..1: position between the previous and the current simulation tick
    const std::string& quality() const { return quality_; }
    // extra sprites supplied by the host each frame (particles)
    std::vector<SpriteQuad> extraSprites;
    // exposed for UI drawing
    Shader* shader(const std::string& name);
    Texture& tex(const std::string& name);

    // helpers shared with sprite builders
    void addSprite(std::vector<SpriteQuad>& out, const Vec3& pos, const Quat& rot, float hw, float hh,
                   const float rgba[4], int tile, SpriteMode mode, float blend);
    // quad with explicit UVs (particles): corners (BL,BR,TR,TL) use uv rect u0,v0,u1,v1
    void addUvSprite(std::vector<SpriteQuad>& out, const Vec3& pos, const Quat& rot, float hw, float hh,
                     const float rgba[4], const float uv[4], float blend);
    Vec3 camPos;
    float cameraZ = 0;
    int playerMode = 1; // for the expert/training sprite tints (set by buildSprites and by the host before particles)

private:
    struct GpuBatch {
        DynMesh mesh;
    };
    const Vfs* vfs_ = nullptr;
    std::string quality_ = "high";
    std::minstd_rand shakeRnd_; // camera-shake jitter, kept apart from the gameplay RNG
    int shakeTick_ = -1;        // level tick the jitter below was drawn for
    float shakeJit_[3][2] = {}; // per eye (-1, 0, 1): x/y jitter, redrawn once per game tick
    std::string locale_ = "en";
    std::unordered_map<std::string, std::unique_ptr<Shader>> shaders_;
    std::unordered_map<std::string, Texture> textures_;
    std::vector<std::unique_ptr<GpuBatch>> batches_;
    Texture blob_;
    DynMesh reflMaskMesh_, reflGeomMesh_;
    DynMesh dynPlain_, dynGlass_, dynGlassTex_, dynMetal_, dynDebris_, dynSprites_, dynShadow_, dynBall_, dynEdges_,
        dynWater_, quad_;
    DynMesh splitQuad_[2]; // split-screen eye quads
    Fbo refr_[2];
    Fbo scene_, dofA_, dofB_; // high quality: scene target + quarter-res near-field blur ping-pong
    int refrRead_ = 0;
    float lineWidth2_ = 1.0f; // glLineWidth(2) clamped to what the core context supports
    uint32_t laserRng_ = 1;
    int lastW_ = 0, lastH_ = 0;
    float lowerFog_[4] = {1, 1, 1, 1}, upperFog_[4] = {1, 1, 1, 1};
    Mat4 proj_, view_;
    std::vector<SpriteQuad> sprites_;
    std::vector<uint8_t> ballVerts_; // chrome discs (ballreflection): balls at medium/high, powerups at every quality
    std::vector<uint32_t> ballIdx_;
    size_t spriteMirStart_ = 0, spriteMirCount_ = 0, spriteIdxCount_ = 0, spriteDoorStart_ = 0, spriteDoorCount_ = 0;
    std::unordered_map<std::string, std::unique_ptr<DynMesh>> menuMeshes_;

    void computeFog(Level& lv);
    void loadBatch(Level& lv, MeshBatch& b);
    void drawRoom(Level& lv, Room& room, Shader& s);
    Shader* qualityShader(const std::string& name, bool low); // `name`low at low quality, falling back to `name`
    void drawnPose(const Body& b, Vec3& pos, Quat& rot) const;
    void buildShapes(Level& lv, std::vector<uint8_t>& plain, std::vector<uint8_t>& glass,
                     std::vector<uint8_t>& glassTex, std::vector<uint8_t>& metal, std::vector<uint8_t>* edges);
    void buildRopes(Level& lv, std::vector<uint8_t>& metal);
    void buildSprites(Level& lv);
    void addBallSprites(Level& lv);
    void addRoomSprites(Level& lv);
    void addButtonSprites(Level& lv);
    void addHintSprites(Level& lv);
    void addBodyDecalSprites(Level& lv);
    void addScoreSprites(Level& lv);
    void addLaserSprites(Level& lv);
    void buildShadows(Level& lv, std::vector<uint8_t>& out);
    void useFog(Shader& s);
    void drawBackground(Shader& clear);
    void drawRefractionBuffer(Level& lv, bool split);
    void drawDofBuffer(Level& lv);
    void drawView(Level& lv, int w, int h, int eye); // eye -1: whole window; 0/1: split-screen portrait eye into scene_
    void prepareTargets(int w, int h, bool useScene);
    void setupCamera(Level& lv, int w, int h, int eye);
    void preloadBatches(Level& lv);
    void buildGeometry(Level& lv, bool low);
    void buildDebris(Level& lv, std::vector<uint8_t>& out);
    void drawRooms(Level& lv, bool low);
    void drawPlainShapes(const Mat4& mvp);
    void drawGlass(bool low, const Mat4& mvp);
    void drawMetal(bool low, const Mat4& mvp);
    void uploadSprites();
    void drawFloorReflections(Level& lv, const Mat4& mvp);
    void drawSoftShadows(const Mat4& mvp);
    void drawWater(Level& lv, const Mat4& mvp);
    void drawBallDiscs(const Mat4& mvp);
    void drawSprites(bool low, const Mat4& mvp);
    void drawBestDistance(Level& lv, const Mat4& mvp);
    void drawFullscreen(const char* shaderName, GLuint tex0, bool nearest0, GLuint tex1 = 0,
                        const DynMesh* mesh = nullptr);
    void sortSprites();
    void addReflection(const Vec3& pos, float radius, const float rgb[3]);
};

} // namespace sh
