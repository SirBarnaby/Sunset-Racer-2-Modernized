// sr2fx post effects. Runs inside the game's main scene, just before the HUD:
// the scene was drawn into m_scene with m_depthTex (INTZ) as its depth buffer;
// the passes read both and write the result into the game's back buffer.
#include "post.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#include "VS_Quad.h"
#include "VS_ObjVel.h"
#include "PS_Copy.h"
#include "PS_LinZ.h"
#include "PS_Debug.h"
#include "PS_AO_Lo.h"
#include "PS_AO_Hi.h"
#include "PS_AOBlur.h"
#include "PS_SunMask.h"
#include "PS_SunBlur.h"
#include "PS_Composite.h"
#include "PS_AOMul.h"
#include "PS_CamVel.h"
#include "PS_ObjVel.h"
#include "PS_TileMax.h"
#include "PS_NeighborMax.h"
#include "PS_Reconstruct.h"
#include "PS_FXAA.h"

static const D3DFORMAT FMT_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

// Ambient occlusion, in the game's world units (a car is about 380 long).
static const float AO_RADIUS = 120.0f;       // how far a surface looks for occluders
static const float AO_STRENGTH = 0.7f;       // 0..1 of the full horizon-based shade
static const float AO_THICKNESS = 0.5f;      // assumed depth of occluders, times the radius
static const float AO_MAX_SCREEN = 0.06f;    // radius cap, fraction of the screen height
static const float AO_FADE_START = 4000.0f;  // view depth where AO starts to fade out
static const float AO_FADE_END = 9000.0f;    // and where it is gone

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------ matrices (row vectors, double)

struct M4 { double m[4][4]; };

static M4 from(const D3DMATRIX& a) {
    M4 r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][j];
    return r;
}
static M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
static bool inverse(const M4& a, M4& out) {
    double t[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(t[r][c]) > fabs(t[p][c])) p = r;
        if (fabs(t[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) std::swap(t[p][j], t[c][j]);
        double d = t[c][c];
        for (int j = 0; j < 8; j++) t[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) {
            double f = t[r][c];
            if (f != 0) for (int j = 0; j < 8; j++) t[r][j] -= f * t[c][j];
        }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out.m[i][j] = t[i][j + 4];
    return true;
}
// HLSL keeps float4x4 constants column-major: upload the transpose.
static void put_matrix(float* dst, const M4& a) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) dst[i * 4 + j] = (float)a.m[j][i];
}

// ------------------------------------------------------------ setup

Post::Post(IDirect3DDevice9* dev) : m_dev(dev) {
    if (!create_shaders()) {
        log_line("post: shader creation failed, effects off for this device");
        m_failed = true;
    }
}

Post::~Post() {
    end_redirect();
    on_lost();
    rel(m_vsQuad); rel(m_vsObj);
    rel(m_psCopy); rel(m_psLinZ); rel(m_psDebug); rel(m_psAO); rel(m_psAOHi); rel(m_psAOBlur); rel(m_psSunMask); rel(m_psSunBlur); rel(m_psComposite); rel(m_psAOMul);
    rel(m_psCamVel); rel(m_psObjVel); rel(m_psTileMax); rel(m_psNeighborMax); rel(m_psReconstruct); rel(m_psFXAA);
    rel(m_declQuad); rel(m_declObj);
}

bool Post::create_shaders() {
    HRESULT hr = S_OK;
#define VS(var, code) if (SUCCEEDED(hr)) hr = m_dev->CreateVertexShader((const DWORD*)code, &var)
#define PS(var, code) if (SUCCEEDED(hr)) hr = m_dev->CreatePixelShader((const DWORD*)code, &var)
    VS(m_vsQuad, g_VS_Quad); VS(m_vsObj, g_VS_ObjVel);
    PS(m_psCopy, g_PS_Copy); PS(m_psLinZ, g_PS_LinZ); PS(m_psDebug, g_PS_Debug); PS(m_psAO, g_PS_AO_Lo); PS(m_psAOHi, g_PS_AO_Hi); PS(m_psAOBlur, g_PS_AOBlur); PS(m_psSunMask, g_PS_SunMask);
    PS(m_psSunBlur, g_PS_SunBlur); PS(m_psComposite, g_PS_Composite); PS(m_psAOMul, g_PS_AOMul); PS(m_psCamVel, g_PS_CamVel);
    PS(m_psObjVel, g_PS_ObjVel); PS(m_psTileMax, g_PS_TileMax); PS(m_psNeighborMax, g_PS_NeighborMax);
    PS(m_psReconstruct, g_PS_Reconstruct); PS(m_psFXAA, g_PS_FXAA);
#undef VS
#undef PS
    static const D3DVERTEXELEMENT9 quad[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    static const D3DVERTEXELEMENT9 obj[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()};
    if (SUCCEEDED(hr)) hr = m_dev->CreateVertexDeclaration(quad, &m_declQuad);
    if (SUCCEEDED(hr)) hr = m_dev->CreateVertexDeclaration(obj, &m_declObj);
    if (FAILED(hr)) log_line("post: create_shaders hr 0x%lx", hr);
    return SUCCEEDED(hr);
}

void Post::release(RT& r) { rel(r.surf); rel(r.tex); r.w = r.h = 0; }

bool Post::ensure(RT& r, UINT w, UINT h, D3DFORMAT f) {
    if (r.tex && r.w == w && r.h == h) return true;
    release(r);
    HRESULT hr = m_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &r.tex, nullptr);
    if (SUCCEEDED(hr)) hr = r.tex->GetSurfaceLevel(0, &r.surf);
    if (FAILED(hr)) {
        log_line("post: render target %ux%u format %lu failed 0x%lx", w, h, (DWORD)f, hr);
        release(r);
        return false;
    }
    r.w = w; r.h = h;
    return true;
}

void Post::on_lost() {
    release(m_scene); release(m_temp); release(m_linz); release(m_ao[0]); release(m_ao[1]); release(m_sun[0]); release(m_sun[1]);
    release(m_vel); release(m_tileH); release(m_tile); release(m_nmax);
    rel(m_depthSurf); rel(m_depthTex);
    rel(m_state);
    timers_release();
    m_w = m_h = 0;
}

bool Post::ensure_targets(const Settings& s) {
    if (m_depthTex) {
        D3DSURFACE_DESC d;
        m_depthSurf->GetDesc(&d);
        if (d.Width != m_w || d.Height != m_h) { rel(m_depthSurf); rel(m_depthTex); }
    }
    if (!m_depthTex) {
        HRESULT hr = m_dev->CreateTexture(m_w, m_h, 1, D3DUSAGE_DEPTHSTENCIL, FMT_INTZ, D3DPOOL_DEFAULT, &m_depthTex, nullptr);
        if (SUCCEEDED(hr)) hr = m_depthTex->GetSurfaceLevel(0, &m_depthSurf);
        if (FAILED(hr)) { log_line("post: INTZ depth %ux%u failed 0x%lx", m_w, m_h, hr); rel(m_depthTex); return false; }
        log_line("post: targets for %ux%u", m_w, m_h);
    }
    if (!ensure(m_scene, m_w, m_h, m_fmt) || !ensure(m_temp, m_w, m_h, m_fmt)) return false;
    if (s.ao) {
        UINT w = s.ao >= 2 ? m_w : (m_w + 1) / 2, h = s.ao >= 2 ? m_h : (m_h + 1) / 2;
        if (!ensure(m_ao[0], w, h, D3DFMT_G16R16F) || !ensure(m_ao[1], w, h, D3DFMT_G16R16F) || !ensure(m_linz, w, h, D3DFMT_R32F)) return false;
        m_aoW = w; m_aoH = h;
    } else { release(m_ao[0]); release(m_ao[1]); release(m_linz); }
    if (s.sunshafts) {
        UINT w = (m_w + 3) / 4, h = (m_h + 3) / 4;
        if (!ensure(m_sun[0], w, h, m_fmt) || !ensure(m_sun[1], w, h, m_fmt)) return false;
    } else { release(m_sun[0]); release(m_sun[1]); }
    if (s.motionblur) {
        m_K = std::max(8, std::min(64, (int)lround(m_h * 0.03)));
        UINT tw = (m_w + m_K - 1) / m_K, th = (m_h + m_K - 1) / m_K;
        if (!ensure(m_vel, m_w, m_h, D3DFMT_G16R16F) || !ensure(m_tileH, tw, m_h, D3DFMT_G16R16F) ||
            !ensure(m_tile, tw, th, D3DFMT_G16R16F) || !ensure(m_nmax, tw, th, D3DFMT_G16R16F)) return false;
    } else { release(m_vel); release(m_tileH); release(m_tile); release(m_nmax); }
    if (!m_state && FAILED(m_dev->CreateStateBlock(D3DSBT_ALL, &m_state))) { log_line("post: state block failed"); return false; }
    return true;
}

// ------------------------------------------------------------ redirect

bool Post::begin_redirect(const Settings& s) {
    if (m_failed || m_redirected) return false;
    if (FAILED(m_dev->GetRenderTarget(0, &m_gameRT)) || !m_gameRT) return false;
    if (FAILED(m_dev->GetDepthStencilSurface(&m_gameDS))) m_gameDS = nullptr;
    D3DSURFACE_DESC d;
    m_gameRT->GetDesc(&d);
    if (d.Width != m_w || d.Height != m_h) {
        on_lost();
        m_w = d.Width; m_h = d.Height;
    }
    m_fmt = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) ? d.Format : D3DFMT_A8R8G8B8;
    if (!ensure_targets(s)) {
        log_line("post: could not make targets, effects off for this device");
        m_failed = true;
        on_lost();
        rel(m_gameRT); rel(m_gameDS);
        return false;
    }
    if (FAILED(m_dev->SetRenderTarget(0, m_scene.surf)) || FAILED(m_dev->SetDepthStencilSurface(m_depthSurf))) {
        m_dev->SetRenderTarget(0, m_gameRT);
        m_dev->SetDepthStencilSurface(m_gameDS);
        rel(m_gameRT); rel(m_gameDS);
        return false;
    }
    m_redirected = true;
    m_aoEarly = false;
    if (g_dev) { timers_begin(); mark(0); }
    return true;
}

void Post::restore_game_targets() {
    m_dev->SetRenderTarget(0, m_gameRT);
    m_dev->SetDepthStencilSurface(m_gameDS);
    rel(m_gameRT); rel(m_gameDS);
    m_redirected = false;
}

void Post::end_redirect() {
    if (!m_redirected) return;
    restore_game_targets();
    if (m_cur) timers_end();
}

// ------------------------------------------------------------ pass helpers

void Post::common_states() {
    IDirect3DDevice9* d = m_dev;
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_DEPTHBIAS, 0);
    d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CLIPPING, TRUE);
    d->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    d->SetVertexDeclaration(m_declQuad);
    d->SetVertexShader(m_vsQuad);
    for (DWORD i = 0; i < 4; i++) {
        d->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, 0);
        d->SetSamplerState(i, D3DSAMP_MAXMIPLEVEL, 0);
        d->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, 0);
        d->SetTexture(i, nullptr);
    }
}

void Post::set_tex(DWORD stage, IDirect3DBaseTexture9* t, bool linear) {
    m_dev->SetTexture(stage, t);
    D3DTEXTUREFILTERTYPE f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    m_dev->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
    m_dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
}

void Post::quad(IDirect3DSurface9* target, UINT tw, UINT th) {
    m_dev->SetRenderTarget(0, target);
    float off[4] = {-1.0f / tw, 1.0f / th, 0, 0};
    m_dev->SetVertexShaderConstantF(0, off, 1);
    static const float v[] = {
        -1, 1, 0, 1, 0, 0,
        1, 1, 0, 1, 1, 0,
        -1, -1, 0, 1, 0, 1,
        1, -1, 0, 1, 1, 1,
    };
    m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, 24);
}

// ------------------------------------------------------------ the effects

// Linear depth, AO and its blur into m_ao[0]. Needs common_states() and the
// depth texture unbound as depth; leaves the render target on m_ao[0].
void Post::compute_ao(const Settings& s, const FrameData& f) {
    IDirect3DDevice9* d = m_dev;
    bool high = s.ao >= 2;
    // High is the same look at full resolution with more directions, never
    // a darker one: radius, strength and fades are shared.
    float c[3][4] = {
        {AO_RADIUS, AO_STRENGTH, m_aoH * AO_MAX_SCREEN, AO_RADIUS * AO_THICKNESS},
        {0.0f, f.fogStart, f.fogEnd, f.fogOn ? 1.0f : 0.0f},
        {(float)m_aoW, (float)m_aoH, 1.0f / m_aoW, 1.0f / m_aoH},
    };
    float c6[4] = {AO_FADE_START, AO_FADE_END, high ? 0.0f : 1.0f, 0.0f};
    d->SetPixelShaderConstantF(2, c[0], 3);
    d->SetPixelShaderConstantF(6, c6, 1);
    d->SetPixelShader(m_psLinZ);
    set_tex(0, m_depthTex, false);
    quad(m_linz.surf, m_aoW, m_aoH);
    d->SetPixelShader(high ? m_psAOHi : m_psAO);
    set_tex(0, m_linz.tex, false);
    quad(m_ao[0].surf, m_aoW, m_aoH);
    d->SetPixelShader(m_psAOBlur);
    float bh[4] = {1.0f / m_aoW, 0, 0, 0}, bv[4] = {0, 1.0f / m_aoH, 0, 0};
    d->SetPixelShaderConstantF(5, bh, 1);
    set_tex(0, m_ao[0].tex, false);
    quad(m_ao[1].surf, m_aoW, m_aoH);
    d->SetPixelShaderConstantF(5, bv, 1);
    set_tex(0, m_ao[1].tex, false);
    quad(m_ao[0].surf, m_aoW, m_aoH);
}

// The opaque part of the main scene is done and the see-through draws are about
// to start: shade the scene with AO now, so smoke, rain, glows and glass go on
// top unshaded. Keeps the redirect (scene colour + INTZ depth) for the rest.
void Post::apply_ao_early(const Settings& s, const FrameData& f) {
    if (!m_redirected || m_aoEarly || !s.ao || !m_ao[0].tex) return;
    IDirect3DDevice9* d = m_dev;
    if (m_cur) mark(1);
    m_state->Capture();
    common_states();
    d->SetDepthStencilSurface(nullptr);
    const D3DMATRIX& P = f.proj;
    float proj[4] = {P._11, P._22, P._33, P._43};
    float res[4] = {(float)m_w, (float)m_h, 1.0f / m_w, 1.0f / m_h};
    d->SetPixelShaderConstantF(0, proj, 1);
    d->SetPixelShaderConstantF(1, res, 1);
    compute_ao(s, f);
    float c[4] = {1.0f, 0.0f, (float)m_aoW, (float)m_aoH};
    d->SetPixelShaderConstantF(2, c, 1);
    d->SetPixelShader(m_psAOMul);
    set_tex(1, m_ao[0].tex, false);
    set_tex(3, m_depthTex, false);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    quad(m_scene.surf, m_w, m_h);
    for (DWORD i = 0; i < 4; i++) d->SetTexture(i, nullptr);
    d->SetRenderTarget(0, m_scene.surf);
    d->SetDepthStencilSurface(m_depthSurf);
    m_state->Apply();
    m_aoEarly = true;
    if (m_cur) mark(0);
}


void Post::apply(const Settings& s, const FrameData& f) {
    if (!m_redirected) return;
    IDirect3DDevice9* d = m_dev;
    if (m_cur) mark(1);
    m_state->Capture();
    common_states();
    d->SetDepthStencilSurface(nullptr);

    const D3DMATRIX& P = f.proj;
    float proj[4] = {P._11, P._22, P._33, P._43};
    float res[4] = {(float)m_w, (float)m_h, 1.0f / m_w, 1.0f / m_h};
    d->SetPixelShaderConstantF(0, proj, 1);
    d->SetPixelShaderConstantF(1, res, 1);

    // ---- ambient occlusion (unless it already went in before the see-through draws)
    bool doAO = s.ao && m_ao[0].tex && !m_aoEarly;
    if (doAO) compute_ao(s, f);
    if (m_cur) mark(2);

    // ---- sun shafts
    bool doSun = false;
    float shaftColor[4] = {0, 0, 0, 0};
    if (s.sunshafts && m_sun[0].tex && f.haveSun && f.sun.Type == D3DLIGHT_DIRECTIONAL) {
        const D3DMATRIX& V = f.view;
        double lx = -f.sun.Direction.x, ly = -f.sun.Direction.y, lz = -f.sun.Direction.z;
        double ll = sqrt(lx * lx + ly * ly + lz * lz);
        if (ll > 1e-6) {
            lx /= ll; ly /= ll; lz /= ll;
            double vx = lx * V._11 + ly * V._21 + lz * V._31;
            double vy = lx * V._12 + ly * V._22 + lz * V._32;
            double vz = lx * V._13 + ly * V._23 + lz * V._33;
            m_sunDbg[0] = (float)vx; m_sunDbg[1] = (float)vy; m_sunDbg[2] = (float)vz;
            if (vz > 0.02) {
                double nx = (vx * P._11 + vz * P._31) / vz, ny = (vy * P._22 + vz * P._32) / vz;
                double u = nx * 0.5 + 0.5, v = 0.5 - ny * 0.5;
                m_sunDbg[3] = (float)u; m_sunDbg[4] = (float)v;
                double edge = std::max(fabs(u - 0.5), fabs(v - 0.5));
                double fade = std::min(1.0, vz * 5.0) * std::max(0.0, std::min(1.0, (1.0 - edge) / 0.5));
                double lum = 0.3 * f.sun.Diffuse.r + 0.59 * f.sun.Diffuse.g + 0.11 * f.sun.Diffuse.b;
                if (fade > 0.01 && lum > 0.02) {
                    doSun = true;
                    float k = (float)(0.75 * fade);
                    shaftColor[0] = std::min(1.0f, f.sun.Diffuse.r) * k;
                    shaftColor[1] = std::min(1.0f, f.sun.Diffuse.g) * k;
                    shaftColor[2] = std::min(1.0f, f.sun.Diffuse.b) * k;
                    UINT sw = m_sun[0].w, sh = m_sun[0].h;
                    float c1[2][4] = {{(float)u, (float)v, (float)m_w / m_h, 0.35f}, {1, 1, 1, 0.75f}};
                    d->SetPixelShaderConstantF(2, c1[0], 2);
                    d->SetPixelShader(m_psSunMask);
                    set_tex(0, m_scene.tex, true);
                    set_tex(1, m_depthTex, false);
                    quad(m_sun[0].surf, sw, sh);
                    d->SetTexture(1, nullptr);
                    d->SetPixelShader(m_psSunBlur);
                    double decay = 0.93, wsum = 0, il = 1;
                    for (int i = 0; i < 16; i++) { wsum += il; il *= decay; }
                    float b1[4] = {1.0f, (float)decay, (float)(1.2 / wsum), 0.75f};
                    d->SetPixelShaderConstantF(3, b1, 1);
                    set_tex(0, m_sun[0].tex, true);
                    quad(m_sun[1].surf, sw, sh);
                    float b2[4] = {1.0f / 16.0f, 1.0f, 1.0f / 16.0f, 0.75f};
                    d->SetPixelShaderConstantF(3, b2, 1);
                    set_tex(0, m_sun[1].tex, true);
                    quad(m_sun[0].surf, sw, sh);
                }
            }
        }
    }
    if (m_cur) mark(3);

    // ---- chain: composite (AO, shafts) -> motion blur -> post AA -> back buffer
    bool doMB = s.motionblur && m_vel.tex && f.havePrev && !s.env_paused && s.blurstrength > 0 && f.dt > 0;
    bool doAA = wants_post_aa(s);
    RT* src = &m_scene;
    RT* spare = &m_temp;
    auto target_for = [&](bool last) -> std::pair<IDirect3DSurface9*, RT*> {
        if (last) return {m_gameRT, nullptr};
        return {spare->surf, spare};
    };
    int remaining = (doAO || doSun ? 1 : 0) + (doMB ? 1 : 0) + (doAA ? 1 : 0);
    bool anyRan = m_aoEarly;

    if (doAO || doSun) {
        float c[2][4] = {{doAO ? 1.0f : 0.0f, doSun ? 1.0f : 0.0f, (float)m_aoW, (float)m_aoH},
                         {shaftColor[0], shaftColor[1], shaftColor[2], 0}};
        d->SetPixelShaderConstantF(1, res, 1);
        d->SetPixelShaderConstantF(2, c[0], 2);
        d->SetPixelShader(m_psComposite);
        set_tex(0, src->tex, false);
        set_tex(1, doAO ? m_ao[0].tex : nullptr, false);
        set_tex(2, doSun ? m_sun[0].tex : nullptr, true);
        set_tex(3, m_depthTex, false);
        auto t = target_for(--remaining == 0);
        quad(t.first, m_w, m_h);
        for (DWORD i = 1; i < 4; i++) d->SetTexture(i, nullptr);
        if (t.second) std::swap(src, spare);
    }
    if (m_cur) mark(4);

    if (doMB) {
        double ref = 1.0 / 60.0;
        double scale = s.blurstrength / 100.0 * std::min(4.0, std::max(0.25, ref / f.dt));
        float mb[4] = {(float)scale, (float)m_K, (float)m_K, 15.0f};
        // camera: current NDC + depth -> previous clip
        M4 vp = mul(from(f.view), from(f.proj));
        M4 vpPrev = mul(from(f.prevView), from(f.prevProj));
        M4 inv;
        float reproj[16];
        bool okInv = inverse(vp, inv);
        put_matrix(reproj, okInv ? mul(inv, vpPrev) : vp);
        d->SetPixelShaderConstantF(2, reproj, 4);
        d->SetPixelShaderConstantF(6, mb, 1);
        d->SetPixelShader(m_psCamVel);
        set_tex(0, m_depthTex, false);
        quad(m_vel.surf, m_w, m_h);
        d->SetTexture(0, nullptr);

        // moving objects
        if (okInv && !f.objs.empty()) {
            d->SetDepthStencilSurface(m_depthSurf);
            d->SetRenderState(D3DRS_ZENABLE, TRUE);
            d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            float bias = -0.000002f, slope = -1.0f;
            d->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&bias);
            d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&slope);
            d->SetVertexDeclaration(m_declObj);
            d->SetVertexShader(m_vsObj);
            d->SetPixelShader(m_psObjVel);
            M4 view = from(f.view), proj = from(f.proj), vpp = vpPrev;
            for (const ObjDraw& o : f.objs) {
                M4 w = from(o.world);
                float c[32];
                M4 wvp = mul(w, vp);
                put_matrix(c, wvp);
                if (s.motionblur >= 2) {
                    // Where this point was last frame: the object's world eased back by
                    // its share of the last change, seen by last frame's camera. Exact
                    // reprojection, so a car moving with the camera stays sharp.
                    M4 pw = from(o.prevWorld), lw;
                    for (int r = 0; r < 4; r++)
                        for (int k = 0; k < 4; k++) lw.m[r][k] = w.m[r][k] + (pw.m[r][k] - w.m[r][k]) * o.share;
                    put_matrix(c + 16, mul(lw, vpp));
                } else {
                    // camera-only blur: moving objects stay sharp
                    put_matrix(c + 16, wvp);
                }
                d->SetVertexShaderConstantF(4, c, 8);
                d->DrawIndexedPrimitiveUP(o.type, 0, o.numVertices, o.primCount, &f.idx[o.firstIndex], D3DFMT_INDEX16,
                                          &f.pos[(size_t)o.firstVertex * 3], 12);
            }
            d->SetDepthStencilSurface(nullptr);
            d->SetRenderState(D3DRS_ZENABLE, FALSE);
            d->SetRenderState(D3DRS_DEPTHBIAS, 0);
            d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
            d->SetVertexDeclaration(m_declQuad);
            d->SetVertexShader(m_vsQuad);
            (void)view; (void)proj;
        }
        if (m_cur) mark(5);

        // tiles
        UINT tw = m_tile.w, th = m_tile.h;
        d->SetPixelShader(m_psTileMax);
        float t1[2][4] = {{1.0f / m_w, 0, (float)m_K, 0}, {(float)(tw * m_K) / m_w, 1, 0, 0}};
        d->SetPixelShaderConstantF(8, t1[0], 2);
        set_tex(0, m_vel.tex, false);
        quad(m_tileH.surf, tw, m_h);
        float t2[2][4] = {{0, 1.0f / m_h, (float)m_K, 0}, {1, (float)(th * m_K) / m_h, 0, 0}};
        d->SetPixelShaderConstantF(8, t2[0], 2);
        set_tex(0, m_tileH.tex, false);
        quad(m_tile.surf, tw, th);
        float t3[4] = {1.0f / tw, 1.0f / th, 0, 0};
        d->SetPixelShaderConstantF(8, t3, 1);
        d->SetPixelShader(m_psNeighborMax);
        set_tex(0, m_tile.tex, false);
        quad(m_nmax.surf, tw, th);

        // gather
        float c7[4] = {(float)m_w / (m_K * tw), (float)m_h / (m_K * th), 0, 0};
        d->SetPixelShaderConstantF(0, proj, 1);
        d->SetPixelShaderConstantF(1, res, 1);
        d->SetPixelShaderConstantF(6, mb, 1);
        d->SetPixelShaderConstantF(7, c7, 1);
        d->SetPixelShader(m_psReconstruct);
        set_tex(0, src->tex, true);
        set_tex(1, m_vel.tex, false);
        set_tex(2, m_nmax.tex, false);
        set_tex(3, m_depthTex, false);
        auto t = target_for(--remaining == 0);
        quad(t.first, m_w, m_h);
        for (DWORD i = 1; i < 4; i++) d->SetTexture(i, nullptr);
        if (t.second) std::swap(src, spare);
    }
    if (m_cur) mark(6);

    if (doAA) {
        d->SetPixelShaderConstantF(1, res, 1);
        d->SetPixelShader(m_psFXAA);
        set_tex(0, src->tex, true);
        quad(m_gameRT, m_w, m_h);
        --remaining;
    } else if (src == &m_scene && !(doAO || doSun) && !doMB) {
        // nothing else ran this frame (AO already in, or motion blur paused): plain copy
        (void)anyRan;
        d->SetPixelShader(m_psCopy);
        set_tex(0, src->tex, false);
        quad(m_gameRT, m_w, m_h);
    }
    if (g_dev && s.fxdebug) {
        IDirect3DBaseTexture9* t = s.fxdebug == 1 ? (IDirect3DBaseTexture9*)m_ao[0].tex
                                 : s.fxdebug == 2 ? (IDirect3DBaseTexture9*)m_vel.tex : (IDirect3DBaseTexture9*)m_sun[0].tex;
        if (t) {
            float dbg[4] = {(float)s.fxdebug, (float)m_K, 0, 0};
            d->SetPixelShaderConstantF(2, dbg, 1);
            d->SetPixelShader(m_psDebug);
            set_tex(0, t, false);
            quad(m_gameRT, m_w, m_h);
        }
    }
    if (m_cur) mark(7);

    d->SetTexture(0, nullptr);
    restore_game_targets();
    m_state->Apply();
    if (m_cur) timers_end();
}

void Post::resolve() {
    if (!m_redirected) return;
    m_state->Capture();
    common_states();
    m_dev->SetDepthStencilSurface(nullptr);
    m_dev->SetPixelShader(m_psCopy);
    set_tex(0, m_scene.tex, false);
    quad(m_gameRT, m_w, m_h);
    m_dev->SetTexture(0, nullptr);
    restore_game_targets();
    m_state->Apply();
    if (m_cur) timers_end();
}

// ------------------------------------------------------------ GPU timing (dev log)

static const char* const kLabels[8] = {"scene", "ao", "shafts", "composite", "mb-velocity", "mb-gather", "aa/out", ""};

void Post::timers_begin() {
    TimerSet& t = m_timers[m_timerIdx];
    if (t.pending) timers_collect();
    if (t.pending) { m_cur = nullptr; return; }   // GPU too far behind: skip this frame
    if (!t.disjoint) {
        if (FAILED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &t.disjoint)) ||
            FAILED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &t.freq))) { m_cur = nullptr; return; }
        for (auto& q : t.ts) m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q);
    }
    t.used = 0;
    t.disjoint->Issue(D3DISSUE_BEGIN);
    m_cur = &t;
}

void Post::mark(int label) {
    if (!m_cur || m_cur->used >= NMARK || !m_cur->ts[m_cur->used]) return;
    m_cur->label[m_cur->used] = label;
    m_cur->ts[m_cur->used++]->Issue(D3DISSUE_END);
}

void Post::timers_end() {
    if (!m_cur) return;
    m_cur->freq->Issue(D3DISSUE_END);
    m_cur->disjoint->Issue(D3DISSUE_END);
    m_cur->pending = true;
    m_cur = nullptr;
    m_timerIdx = (m_timerIdx + 1) % NRING;
    timers_collect();
}

void Post::timers_collect() {
    for (auto& t : m_timers) {
        if (!t.pending) continue;
        BOOL disjoint = TRUE;
        UINT64 freq = 0;
        if (t.disjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK) continue;
        if (t.freq->GetData(&freq, sizeof freq, 0) != S_OK) continue;
        UINT64 v[NMARK];
        bool ready = true;
        for (int i = 0; i < t.used; i++) if (t.ts[i]->GetData(&v[i], sizeof v[i], 0) != S_OK) { ready = false; break; }
        if (!ready) continue;
        t.pending = false;
        if (disjoint || !freq) continue;
        for (int i = 1; i < t.used; i++) {
            int lab = t.label[i - 1];
            if (lab >= 0 && lab < 8) m_tsum[lab] += (double)(v[i] - v[i - 1]) * 1000.0 / freq;
        }
        m_tcount++;
    }
}

void Post::timers_release() {
    for (auto& t : m_timers) {
        rel(t.disjoint); rel(t.freq);
        for (auto& q : t.ts) rel(q);
        t.pending = false;
        t.used = 0;
    }
    m_cur = nullptr;
}

void Post::log_timings() {
    if (!m_tcount) return;
    char b[400];
    int n = snprintf(b, sizeof b, "gpu ms (avg of %d frames):", m_tcount);
    double total = 0;
    for (int i = 0; i < 7; i++) {
        double ms = m_tsum[i] / m_tcount;
        if (i > 0) total += ms;
        n += snprintf(b + n, sizeof b - n, " %s %.2f", kLabels[i], ms);
    }
    snprintf(b + n, sizeof b - n, " | effects total %.2f", total);
    log_line("%s", b);
    log_line("sun view dir %.3f %.3f %.3f  screen uv %.3f %.3f", m_sunDbg[0], m_sunDbg[1], m_sunDbg[2], m_sunDbg[3], m_sunDbg[4]);
    memset(m_tsum, 0, sizeof m_tsum);
    m_tcount = 0;
}
