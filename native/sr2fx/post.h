// sr2fx post effects: owned per device by sr2fx.cpp.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <vector>

// SunsetRacer2-fx.ini, see sr2fx.cpp. Every field 0 = the original game.
struct Settings {
    int renderer;       // 0 DirectX 9, 1 Vulkan (DXVK)
    int aa;             // 0 off, 1 game MSAA, 2 post AA
    int ao;             // 0 off, 1 on (half res), 2 high (full res, more directions)
    int motionblur;     // 0 off, 1 camera, 2 full (per object)
    int blurstrength;   // 0..100
    int sunshafts;      // 0/1
    int lightshadows;   // reserved
    int env_paused;     // live: 1 while the race is paused
    int env_speed;      // live: km/h
    int fxdebug;        // dev file only: 1 AO, 2 velocity, 3 shafts buffer on screen
};

void log_line(const char* fmt, ...);
extern bool g_dev;

// True when the main 3D scene is drawn into the layer's own single-sample
// targets (readable depth). The back buffer keeps the game's MSAA for the menus,
// HUD and text; the 3D scene itself then gets post AA in its place, because
// D3D9 cannot read multisampled depth.
inline bool wants_depth_fx(const Settings& s) { return s.ao || s.motionblur || s.sunshafts; }
inline bool wants_post(const Settings& s) { return wants_depth_fx(s) || s.aa == 2; }
inline bool wants_post_aa(const Settings& s) { return s.aa == 2 || (s.aa == 1 && wants_depth_fx(s)); }

// One moving object's triangles from the main scene, kept for the velocity pass.
struct ObjDraw {
    D3DPRIMITIVETYPE type;
    UINT firstVertex, numVertices;   // into FrameData::pos (xyz floats)
    UINT firstIndex, primCount;      // into FrameData::idx (16-bit, relative to firstVertex)
    D3DMATRIX world, prevWorld;      // this frame, before the object's last change
    float share;                     // object motion per frame / per change
};

struct FrameData {
    D3DMATRIX view, proj;                 // main scene camera this frame
    D3DMATRIX prevView, prevProj;         // and last frame
    bool havePrev;
    D3DLIGHT9 sun;                        // light 0
    bool haveSun;
    float fogStart, fogEnd;
    bool fogOn;
    double dt;                            // seconds since the previous frame
    std::vector<float> pos;
    std::vector<WORD> idx;
    std::vector<ObjDraw> objs;
};

class Post {
public:
    explicit Post(IDirect3DDevice9* dev);
    ~Post();
    bool ok() const { return !m_failed; }
    // Default-pool resources go before a Reset and come back on demand.
    void on_lost();
    // Points the device at the layer's scene colour + INTZ depth. False = stay
    // on the game's targets this frame.
    bool begin_redirect(const Settings& s);
    bool redirected() const { return m_redirected; }
    IDirect3DSurface9* game_rt() const { return m_gameRT; }
    IDirect3DSurface9* game_ds() const { return m_gameDS; }
    // Runs the effects into the game's back buffer and restores its targets.
    void apply(const Settings& s, const FrameData& f);
    // AO into the scene before the see-through draws (once per frame).
    void apply_ao_early(const Settings& s, const FrameData& f);
    bool ao_early_done() const { return m_aoEarly; }
    // Copies the scene to the back buffer unchanged (no main scene this frame).
    void resolve();
    // Restores the game's targets without drawing (abort).
    void end_redirect();
    void log_timings();

private:
    IDirect3DDevice9* m_dev;
    bool m_failed = false;
    bool m_redirected = false;
    bool m_aoEarly = false;     // AO already multiplied into m_scene this frame
    UINT m_w = 0, m_h = 0;
    D3DFORMAT m_fmt = D3DFMT_A8R8G8B8;

    IDirect3DSurface9* m_gameRT = nullptr;   // held while redirected
    IDirect3DSurface9* m_gameDS = nullptr;

    // shaders (survive Reset)
    IDirect3DVertexShader9 *m_vsQuad = nullptr, *m_vsObj = nullptr;
    IDirect3DPixelShader9 *m_psCopy = nullptr, *m_psLinZ = nullptr, *m_psDebug = nullptr, *m_psAO = nullptr, *m_psAOHi = nullptr, *m_psAOBlur = nullptr, *m_psSunMask = nullptr,
                          *m_psSunBlur = nullptr, *m_psComposite = nullptr, *m_psAOMul = nullptr, *m_psCamVel = nullptr, *m_psObjVel = nullptr,
                          *m_psTileMax = nullptr, *m_psNeighborMax = nullptr, *m_psReconstruct = nullptr, *m_psFXAA = nullptr;
    IDirect3DVertexDeclaration9 *m_declQuad = nullptr, *m_declObj = nullptr;

    // default pool
    struct RT { IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; UINT w = 0, h = 0; };
    RT m_scene, m_temp, m_linz, m_ao[2], m_sun[2], m_vel, m_tileH, m_tile, m_nmax;
    IDirect3DTexture9* m_depthTex = nullptr;
    IDirect3DSurface9* m_depthSurf = nullptr;
    IDirect3DStateBlock9* m_state = nullptr;
    UINT m_aoW = 0, m_aoH = 0;
    int m_K = 0;
    float m_sunDbg[5] = {};   // dev log: sun direction in view space, screen uv

    // GPU timing (dev only)
    enum { NMARK = 12, NRING = 4 };
    struct TimerSet { IDirect3DQuery9* disjoint = nullptr; IDirect3DQuery9* freq = nullptr; IDirect3DQuery9* ts[NMARK] = {}; int used = 0; bool pending = false; int label[NMARK]; };
    TimerSet m_timers[NRING];
    int m_timerIdx = 0;
    TimerSet* m_cur = nullptr;
    double m_tsum[8] = {};
    int m_tcount = 0;
    double m_tlast = 0;
    void mark(int label);
    void timers_begin();
    void timers_end();
    void timers_collect();
    void timers_release();

    bool create_shaders();
    void compute_ao(const Settings& s, const FrameData& f);
    bool ensure(RT& r, UINT w, UINT h, D3DFORMAT f);
    void release(RT& r);
    bool ensure_targets(const Settings& s);
    void quad(IDirect3DSurface9* target, UINT tw, UINT th);
    void set_tex(DWORD stage, IDirect3DBaseTexture9* t, bool linear);
    void common_states();
    void restore_game_targets();
};
