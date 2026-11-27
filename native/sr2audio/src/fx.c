// The Steam Audio chain.
//
// Reverb: for each env_room (open road, street canyon, underpass, tunnel) a
// small acoustic scene is built at start-up and Steam Audio's own ray tracer
// simulates the reflections for a listener standing on the road, giving a
// first-order Ambisonics impulse response per room. Those IRs drive Steam
// Audio convolution reverbs; a room change crossfades the reverb input from
// one room to the next while the old room's tail rings out. The Ambisonics
// result is decoded to stereo. hrtf=1 renders sounds the game positions (by
// their pan value) through Steam Audio's HRTF.
//
// env_wet darkens the reverb and the car sounds (high shelf), reverb=0..100
// scales the wet level, env_paused fades the tail out and clears it.
#include "sr2audio.h"
#include <phonon.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define NROOM 4
static const char *roomName[NROOM] = { "open", "street", "underpass", "tunnel" };
static const float roomDur[NROOM] = { 1.2f, 1.8f, 2.0f, 3.5f };     // simulated IR length, seconds
static const float roomGain[NROOM] = { 0.6f, 1.0f, 1.3f, 1.4f };    // tuned against the measured IRs
static const float roomMaxRT[NROOM] = { 0.6f, 0.9f, 1.5f, 3.0f };   // cap on the mid-band late reverb time
// Hybrid reverb: the first HYB_CONV seconds of each IR are convolved (the
// echoes that make a tunnel sound like a tunnel), the rest is Steam Audio's
// parametric reverb driven by the simulated decay times. Full convolution of
// the 3.5 s tunnel IR measured 3.5 ms avg / 8 ms max per 10 ms block here;
// hybrid with 0.1 s of convolution about 0.3-0.5 ms.
#ifndef HYB_CONV
#define HYB_CONV 0.10f
#endif
#ifndef SR_RTYPE
#define SR_RTYPE IPL_REFLECTIONEFFECTTYPE_HYBRID
#endif
#define HYB_OVERLAP 0.25f
#define HYB_SIZE ((int)(HYB_CONV * (1 + HYB_OVERLAP) * SR_RATE))
static const IPLReflectionEffectType RTYPE = SR_RTYPE;

static volatile LONG state;      // 0 not started, 1 building, 2 ready, -1 failed
static IPLContext ctx;
static IPLHRTF hrtf;
static IPLAmbisonicsDecodeEffect decPan;
static IPLBinauralEffect binEff[SR_NBIN];
static struct {
    IPLScene scene; IPLSimulator sim; IPLSource src;
    IPLReflectionEffect eff;
    IPLReflectionEffectParams params;
    volatile LONG ready;
    float feed;                  // current input gain, ramps toward 0 or 1
    int active;                  // tail still ringing
    int quiet;                   // blocks in a row below -96 dB
    double buildMs;
} room[NROOM];
static double buildTotalMs;

// ---- processing state (mixer thread only) ---------------------------------------
static float amb[4][SR_BLOCK], ambSum[4][SR_BLOCK], tmpIn[SR_BLOCK];
static float wetL[SR_BLOCK], wetR[SR_BLOCK], binL[SR_BLOCK], binR[SR_BLOCK];
static float wetGain;            // smoothed master wet gain
static int binWasUsed[SR_NBIN];
static LONGLONG fxTicks, fxBlocks, fxMax;

typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; float db; int set; } Shelf;
static Shelf shelfWet, shelfDie;

// ---- helpers -------------------------------------------------------------------------
static void IPLCALL ipl_log(IPLLogLevel lvl, const char *m) { sr_log("steam audio [%d]: %s", (int)lvl, m); }

static void shelf_set(Shelf *s, float db) {
    // RBJ high shelf, 3 kHz, slope 1
    const double f0 = 3000.0, pi = 3.14159265358979323846;
    double A = pow(10.0, db / 40.0), w = 2 * pi * f0 / SR_RATE, cw = cos(w), sw = sin(w);
    double alpha = sw / 2 * sqrt(2.0), sa = 2 * sqrt(A) * alpha;
    double a0 = (A + 1) - (A - 1) * cw + sa;
    s->b0 = (float)(A * ((A + 1) + (A - 1) * cw + sa) / a0);
    s->b1 = (float)(-2 * A * ((A - 1) + (A + 1) * cw) / a0);
    s->b2 = (float)(A * ((A + 1) + (A - 1) * cw - sa) / a0);
    s->a1 = (float)(2 * ((A - 1) - (A + 1) * cw) / a0);
    s->a2 = (float)(((A + 1) - (A - 1) * cw - sa) / a0);
    s->db = db;
    s->set = 1;
}
static void shelf_run(Shelf *s, int c, float *x) {
    int i; float z1 = s->z1[c], z2 = s->z2[c];
    for (i = 0; i < SR_BLOCK; i++) {          // transposed direct form II
        float in = x[i], o = s->b0 * in + z1;
        z1 = s->b1 * in - s->a1 * o + z2;
        z2 = s->b2 * in - s->a2 * o;
        x[i] = o;
    }
    s->z1[c] = z1; s->z2[c] = z2;
}
static void shelf_toward(Shelf *s, float target) {
    float d, stepMax = 0.3f;                      // dB per 10 ms block
    if (!s->set) shelf_set(s, 0.0f);
    d = target - s->db;
    if (fabsf(d) < 1e-4f) { if (s->db != target) shelf_set(s, target); return; }
    shelf_set(s, s->db + (d > stepMax ? stepMax : d < -stepMax ? -stepMax : d));
}

static float soft_clip(float x) {
    const float t = 0.89f, r = 1.0f - 0.89f;            // knee from -1 dBFS, ceiling at 0 dBFS
    float a = fabsf(x);
    if (a <= t) return x;
    a = t + r * tanhf((a - t) / r);
    return x < 0 ? -a : a;
}

// ---- room geometry ------------------------------------------------------------------------
#define MAXV 512
#define MAXT 512
static IPLVector3 gv[MAXV]; static IPLTriangle gt[MAXT]; static IPLint32 gm[MAXT];
static int nv, nt;

static IPLVector3 V(float x, float y, float z) { IPLVector3 v; v.x = x; v.y = y; v.z = z; return v; }
// a quad, both windings so it reflects from either side
static void quad(IPLVector3 a, IPLVector3 b, IPLVector3 c, IPLVector3 d, int mat) {
    int i0 = nv;
    if (nv + 4 > MAXV || nt + 4 > MAXT) return;
    gv[nv++] = a; gv[nv++] = b; gv[nv++] = c; gv[nv++] = d;
    gt[nt].indices[0] = i0; gt[nt].indices[1] = i0 + 1; gt[nt].indices[2] = i0 + 2; gm[nt++] = mat;
    gt[nt].indices[0] = i0; gt[nt].indices[1] = i0 + 2; gt[nt].indices[2] = i0 + 3; gm[nt++] = mat;
    gt[nt].indices[0] = i0; gt[nt].indices[1] = i0 + 2; gt[nt].indices[2] = i0 + 1; gm[nt++] = mat;
    gt[nt].indices[0] = i0; gt[nt].indices[1] = i0 + 3; gt[nt].indices[2] = i0 + 2; gm[nt++] = mat;
}
static void ground(float x0, float x1, float z0, float z1, float y, int mat) { quad(V(x0, y, z0), V(x1, y, z0), V(x1, y, z1), V(x0, y, z1), mat); }
static void wallx(float x, float z0, float z1, float y0, float y1, int mat) { quad(V(x, y0, z0), V(x, y0, z1), V(x, y1, z1), V(x, y1, z0), mat); }
static void wallz(float z, float x0, float x1, float y0, float y1, int mat) { quad(V(x0, y0, z), V(x1, y0, z), V(x1, y1, z), V(x0, y1, z), mat); }

enum { M_ASPHALT, M_CONCRETE, M_FACADE, M_TILE, NMAT };
static IPLMaterial mats[NMAT] = {
    { { 0.08f, 0.12f, 0.18f }, 0.10f, { 0, 0, 0 } },     // asphalt
    { { 0.02f, 0.03f, 0.05f }, 0.10f, { 0, 0, 0 } },     // bare concrete
    { { 0.03f, 0.05f, 0.08f }, 0.35f, { 0, 0, 0 } },     // facades (brick, glass, windows)
    { { 0.01f, 0.02f, 0.04f }, 0.05f, { 0, 0, 0 } },     // tunnel lining
};

// The road runs along z, the listener looks along -z (Steam Audio's ahead).
static void build_geometry(int r) {
    nv = nt = 0;
    switch (r) {
    case 0:   // open road: ground and a few buildings well away from it
        ground(-300, 300, -300, 300, 0, M_ASPHALT);
        wallx(28, -70, 40, 0, 12, M_FACADE);
        wallx(-45, -30, 90, 0, 20, M_FACADE);
        wallz(-110, -50, 60, 0, 15, M_FACADE);
        break;
    case 1:   // street canyon: 16 m between facades, 18 m high
        ground(-8, 8, -200, 200, 0, M_ASPHALT);
        wallx(-8, -200, 200, 0, 18, M_FACADE);
        wallx(8, -200, 200, 0, 18, M_FACADE);
        break;
    case 2:   // underpass: 22 m wide deck, 6 m clearance, abutments 10 m from the centre line
        ground(-60, 60, -150, 150, 0, M_ASPHALT);
        ground(-10, 10, -11, 11, 6, M_CONCRETE);
        wallx(-10, -11, 11, 0, 6, M_CONCRETE);
        wallx(10, -11, 11, 0, 6, M_CONCRETE);
        break;
    default:  // road tunnel: 11 m x 6.5 m tube, 600 m long
        ground(-5.5f, 5.5f, -300, 300, 0, M_ASPHALT);
        ground(-5.5f, 5.5f, -300, 300, 6.5f, M_TILE);
        wallx(-5.5f, -300, 300, 0, 6.5f, M_TILE);
        wallx(5.5f, -300, 300, 0, 6.5f, M_TILE);
        break;
    }
}

static IPLCoordinateSpace3 listener_space(void) {
    IPLCoordinateSpace3 c;
    c.right = V(1, 0, 0); c.up = V(0, 1, 0); c.ahead = V(0, 0, -1); c.origin = V(0, 1.4f, 0);
    return c;
}

static int build_room(int r) {
    IPLSceneSettings ss; IPLStaticMeshSettings ms; IPLStaticMesh mesh = NULL;
    IPLSimulationSettings sim; IPLSourceSettings srs; IPLSimulationSharedInputs sh; IPLSimulationInputs in;
    IPLSimulationOutputs outp; IPLAudioSettings as; IPLReflectionEffectSettings es;
    LARGE_INTEGER t0, t1, f;
    int irSize = HYB_SIZE, b;
    QueryPerformanceCounter(&t0);

    memset(&ss, 0, sizeof ss); ss.type = IPL_SCENETYPE_DEFAULT;
    if (iplSceneCreate(ctx, &ss, &room[r].scene) != IPL_STATUS_SUCCESS) return 0;
    build_geometry(r);
    memset(&ms, 0, sizeof ms);
    ms.numVertices = nv; ms.numTriangles = nt; ms.numMaterials = NMAT;
    ms.vertices = gv; ms.triangles = gt; ms.materialIndices = gm; ms.materials = mats;
    if (iplStaticMeshCreate(room[r].scene, &ms, &mesh) != IPL_STATUS_SUCCESS) return 0;
    iplStaticMeshAdd(mesh, room[r].scene);
    iplSceneCommit(room[r].scene);
    iplStaticMeshRelease(&mesh);

    memset(&sim, 0, sizeof sim);
    sim.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    sim.sceneType = IPL_SCENETYPE_DEFAULT;
    sim.reflectionType = RTYPE;
    sim.maxNumRays = 16384;
    sim.numDiffuseSamples = 64;
    sim.maxDuration = roomDur[r];
    sim.maxOrder = 1;
    sim.maxNumSources = 1;
    sim.numThreads = 1;
    sim.samplingRate = SR_RATE;
    sim.frameSize = SR_BLOCK;
    if (iplSimulatorCreate(ctx, &sim, &room[r].sim) != IPL_STATUS_SUCCESS) return 0;
    iplSimulatorSetScene(room[r].sim, room[r].scene);
    memset(&srs, 0, sizeof srs); srs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    if (iplSourceCreate(room[r].sim, &srs, &room[r].src) != IPL_STATUS_SUCCESS) return 0;
    iplSourceAdd(room[r].src, room[r].sim);
    iplSimulatorCommit(room[r].sim);

    memset(&sh, 0, sizeof sh);
    sh.listener = listener_space();
    sh.numRays = 16384;
    sh.numBounces = r == 3 ? 96 : 48;
    sh.duration = roomDur[r];
    sh.order = 1;
    sh.irradianceMinDistance = 1.0f;
    iplSimulatorSetSharedInputs(room[r].sim, IPL_SIMULATIONFLAGS_REFLECTIONS, &sh);
    memset(&in, 0, sizeof in);
    in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    in.source = listener_space();
    in.source.origin = V(0, 0.7f, -4.0f);           // the car, a few metres ahead of the camera
    in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
    in.hybridReverbTransitionTime = HYB_CONV;
    in.hybridReverbOverlapPercent = HYB_OVERLAP;
    iplSourceSetInputs(room[r].src, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);
    iplSimulatorRunReflections(room[r].sim);
    memset(&outp, 0, sizeof outp);
    iplSourceGetOutputs(room[r].src, IPL_SIMULATIONFLAGS_REFLECTIONS, &outp);

    as.samplingRate = SR_RATE; as.frameSize = SR_BLOCK;
    memset(&es, 0, sizeof es);
    es.type = RTYPE; es.irSize = irSize; es.numChannels = 4;
    if (iplReflectionEffectCreate(ctx, &as, &es, &room[r].eff) != IPL_STATUS_SUCCESS) return 0;
    room[r].params = outp.reflections;
    room[r].params.type = RTYPE;
    room[r].params.numChannels = 4;
    room[r].params.irSize = irSize;
    sr_log("room %s simulated: reverb times %.2f %.2f %.2f s, eq %.2f %.2f %.2f, delay %d", roomName[r],
           outp.reflections.reverbTimes[0], outp.reflections.reverbTimes[1], outp.reflections.reverbTimes[2],
           outp.reflections.eq[0], outp.reflections.eq[1], outp.reflections.eq[2], outp.reflections.delay);
    if (room[r].params.reverbTimes[1] > roomMaxRT[r]) {      // keep the band ratios, cap the length
        float k = roomMaxRT[r] / room[r].params.reverbTimes[1];
        for (b = 0; b < 3; b++) room[r].params.reverbTimes[b] *= k;
    }

    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    room[r].buildMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    return 1;
}

// Dev only: renders a room's impulse response through its own effect before
// the mixer may use it, and saves it so the reverb can be measured offline.
// (A reflection effect takes over the simulated IR on its first use, so a
// second effect on the same IR would stay silent in its convolution part.)
static void dump_ir(int r) {
    IPLAudioSettings as; IPLAmbisonicsDecodeEffectSettings ds; IPLAmbisonicsDecodeEffect dec = NULL;
    static float a4[4][SR_BLOCK], o2[2][SR_BLOCK], in1[SR_BLOCK];
    as.samplingRate = SR_RATE; as.frameSize = SR_BLOCK;
    memset(&ds, 0, sizeof ds);
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO; ds.hrtf = hrtf; ds.maxOrder = 1;
    if (iplAmbisonicsDecodeEffectCreate(ctx, &as, &ds, &dec) != IPL_STATUS_SUCCESS) return;
    {
        IPLReflectionEffect e = room[r].eff; IPLAmbisonicsDecodeEffectParams dp;
        IPLAudioBuffer bin, bamb, bout; float *pa[4], *po[2], *pi[1];
        char name[64], path[MAX_PATH]; HANDLE h; int blocks = (int)(roomMaxRT[r] * 1.3f * SR_RATE / SR_BLOCK) + 20, k, i;
        wsprintfA(name, "SunsetRacer2-audio-ir-%d-%s.wav", r, roomName[r]);
        sr_exe_dir_file(name, path, MAX_PATH);
        h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { iplAmbisonicsDecodeEffectRelease(&dec); return; }
        {
            struct { char riff[4]; DWORD size; char wave[4]; char fmt[4]; DWORD fsz; WORD tag, ch; DWORD rate, bps; WORD align, bits;
                     char data[4]; DWORD dsz; } hd; DWORD n;
            memcpy(hd.riff, "RIFF", 4); memcpy(hd.wave, "WAVE", 4); memcpy(hd.fmt, "fmt ", 4); memcpy(hd.data, "data", 4);
            hd.fsz = 16; hd.tag = 3; hd.ch = 2; hd.rate = SR_RATE; hd.bps = SR_RATE * 8; hd.align = 8; hd.bits = 32;
            hd.dsz = blocks * SR_BLOCK * 8; hd.size = 36 + hd.dsz;
            WriteFile(h, &hd, sizeof hd, &n, NULL);
        }
        for (k = 0; k < 4; k++) pa[k] = a4[k];
        po[0] = o2[0]; po[1] = o2[1]; pi[0] = in1;
        bin.numChannels = 1; bin.numSamples = SR_BLOCK; bin.data = pi;
        bamb.numChannels = 4; bamb.numSamples = SR_BLOCK; bamb.data = pa;
        bout.numChannels = 2; bout.numSamples = SR_BLOCK; bout.data = po;
        memset(&dp, 0, sizeof dp);
        dp.order = 1; dp.hrtf = hrtf; dp.orientation = listener_space(); dp.binaural = IPL_FALSE;
        iplAmbisonicsDecodeEffectReset(dec);
        LARGE_INTEGER q0, q1, q2, qf; LONGLONG tr = 0, td = 0;
        QueryPerformanceFrequency(&qf);
        for (k = 0; k < blocks; k++) {
            float buf[SR_BLOCK * 2]; DWORD n;
            memset(in1, 0, sizeof in1);
            if (k == 0) in1[0] = 1.0f;
            QueryPerformanceCounter(&q0);
            iplReflectionEffectApply(e, &room[r].params, &bin, &bamb, NULL);
            QueryPerformanceCounter(&q1);
            iplAmbisonicsDecodeEffectApply(dec, &dp, &bamb, &bout);
            QueryPerformanceCounter(&q2);
            tr += q1.QuadPart - q0.QuadPart; td += q2.QuadPart - q1.QuadPart;
            for (i = 0; i < SR_BLOCK; i++) { buf[2 * i] = o2[0][i] * roomGain[r]; buf[2 * i + 1] = o2[1][i] * roomGain[r]; }
            WriteFile(h, buf, sizeof buf, &n, NULL);
        }
        CloseHandle(h);
        {   // let the tail die so the room starts clean (no reset, see above)
            int quiet = 0;
            memset(in1, 0, sizeof in1);
            for (k = 0; k < 2000 && quiet < 10; k++) {
                float pk = 0;
                iplReflectionEffectApply(e, &room[r].params, &bin, &bamb, NULL);
                for (i = 0; i < 4 * SR_BLOCK; i++) { float a = fabsf(a4[i / SR_BLOCK][i % SR_BLOCK]); if (a > pk) pk = a; }
                quiet = pk < 1e-7f ? quiet + 1 : 0;
            }
        }
        sr_log("impulse response of %s -> %s (per 10 ms block: reverb %.3f ms, decode %.3f ms, worker thread)", roomName[r], path,
               tr * 1000.0 / qf.QuadPart / blocks, td * 1000.0 / qf.QuadPart / blocks);
    }
    iplAmbisonicsDecodeEffectRelease(&dec);
}

static DWORD WINAPI worker(LPVOID x) {
    wchar_t p[MAX_PATH];
    IPLContextSettings cs; IPLAudioSettings as; IPLHRTFSettings hs; IPLAmbisonicsDecodeEffectSettings ds;
    IPLBinauralEffectSettings bs; int r, k;
    (void)x;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DWORD tl0 = GetTickCount(), tl1, tl2;
    sr_exe_dir_filew(L"phonon.dll", p, MAX_PATH);
    if (!LoadLibraryW(p)) { sr_log("phonon.dll did not load (%lu): no reverb", GetLastError()); state = -1; return 0; }
    tl1 = GetTickCount();
    memset(&cs, 0, sizeof cs);
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = sr_dev ? ipl_log : NULL;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (iplContextCreate(&cs, &ctx) != IPL_STATUS_SUCCESS) { sr_log("Steam Audio context failed"); state = -1; return 0; }
    as.samplingRate = SR_RATE; as.frameSize = SR_BLOCK;
    memset(&hs, 0, sizeof hs); hs.type = IPL_HRTFTYPE_DEFAULT; hs.volume = 1.0f; hs.normType = IPL_HRTFNORMTYPE_NONE;
    if (iplHRTFCreate(ctx, &as, &hs, &hrtf) != IPL_STATUS_SUCCESS) { sr_log("HRTF failed"); state = -1; return 0; }
    tl2 = GetTickCount();
    memset(&ds, 0, sizeof ds);
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO; ds.hrtf = hrtf; ds.maxOrder = 1;
    if (iplAmbisonicsDecodeEffectCreate(ctx, &as, &ds, &decPan) != IPL_STATUS_SUCCESS) { sr_log("decoder failed"); state = -1; return 0; }
    bs.hrtf = hrtf;
    for (k = 0; k < SR_NBIN; k++)
        if (iplBinauralEffectCreate(ctx, &as, &bs, &binEff[k]) != IPL_STATUS_SUCCESS) { sr_log("binaural failed"); state = -1; return 0; }
    state = 2;                                   // dry, binaural and decoders usable from here
    sr_log("Steam Audio %d.%d.%d ready (load %lu ms, context+HRTF %lu ms), building rooms", STEAMAUDIO_VERSION_MAJOR,
           STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, tl1 - tl0, tl2 - tl1);
    // tunnel first: it is the one that matters most
    for (k = 0; k < NROOM; k++) {
        r = (NROOM - 1 + k) % NROOM;
        if (build_room(r)) {
            if (sr_dev) { SrDevOpts d; sr_devopts_get(&d); if (d.capture) dump_ir(r); }
            InterlockedExchange(&room[r].ready, 1);
            buildTotalMs += room[r].buildMs;
            sr_log("room %s: %.0f ms to simulate (%d triangles, IR %.1f s)", roomName[r], room[r].buildMs, nt, roomDur[r]);
        } else sr_log("room %s failed", roomName[r]);
    }
    return 0;
}

int fx_init(void) {
    static wchar_t p[MAX_PATH];
    if (state == -1) return 0;
    if (state) return 1;
    sr_exe_dir_filew(L"phonon.dll", p, MAX_PATH);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { state = -1; sr_log("phonon.dll is missing: staying on the original sound"); return 0; }
    if (InterlockedCompareExchange(&state, 1, 0) == 0) CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return 1;
}

int fx_ready(void) { return state == 2; }

int fx_busy(void) {
    int r;
    if (wetGain > 0) for (r = 0; r < NROOM; r++) if (room[r].active) return 1;
    return 0;
}

void fx_stats(char *buf, int cap) {
    int r, k = 0;
    k += _snprintf(buf + k, cap - k, "fx %s avg %.3f ms max %.3f ms, wet %.2f, rooms",
                   state == 2 ? "ready" : state == 1 ? "loading" : state < 0 ? "off" : "idle",
                   fxBlocks ? fxTicks * 1000.0 / fxBlocks / 1e7 : 0.0, fxMax / 1e4, wetGain);
    for (r = 0; r < NROOM && k < cap; r++)
        k += _snprintf(buf + k, cap - k, " %s:%s%.2f", roomName[r], room[r].ready ? "" : "(building)", room[r].active ? room[r].feed : 0.0f);
    fxTicks = fxBlocks = fxMax = 0;
}

// ---- per block ---------------------------------------------------------------------------
void fx_process(SrBus *b, const SrSettings *s, float *outL, float *outR) {
    int i, r, k, on = s->steamaudio;
    LARGE_INTEGER t0, t1, f;
    float wetTarget, w = s->env_wet / 100.0f;
    QueryPerformanceCounter(&t0);

    // binaural voices (hrtf=1, positioned by their pan value) join the mono dry bus
    for (k = 0; k < SR_NBIN; k++) {
        IPLAudioBuffer bi, bo; float *pi[1], *po[2]; IPLBinauralEffectParams bp;
        if (!b->binUsed[k]) { binWasUsed[k] = 0; continue; }
        if (state != 2) {        // Steam Audio not loaded: play it centred
            for (i = 0; i < SR_BLOCK; i++) { b->dieL[i] += b->bin[k][i]; b->dieR[i] += b->bin[k][i]; }
            continue;
        }
        if (!binWasUsed[k]) iplBinauralEffectReset(binEff[k]);
        binWasUsed[k] = 1;
        pi[0] = b->bin[k]; po[0] = binL; po[1] = binR;
        bi.numChannels = 1; bi.numSamples = SR_BLOCK; bi.data = pi;
        bo.numChannels = 2; bo.numSamples = SR_BLOCK; bo.data = po;
        memset(&bp, 0, sizeof bp);
        {
            float a = b->binAz[k] * 3.14159265f / 180.0f;
            bp.direction = V(sinf(a), 0, -cosf(a));
        }
        bp.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        bp.spatialBlend = 1.0f;
        bp.hrtf = hrtf;
        iplBinauralEffectApply(binEff[k], &bp, &bi, &bo);
        for (i = 0; i < SR_BLOCK; i++) { b->dieL[i] += binL[i]; b->dieR[i] += binR[i]; }
    }

    // dry: stereo sources untouched, mono ones through the wet-weather shelf
    if (on) {
        shelf_toward(&shelfDie, s->has_env ? -4.0f * w : 0.0f);
        shelf_run(&shelfDie, 0, b->dieL);
        shelf_run(&shelfDie, 1, b->dieR);
    } else if (shelfDie.set) {
        memset(&shelfDie, 0, sizeof shelfDie);
    }
    for (i = 0; i < SR_BLOCK; i++) { outL[i] = b->dryL[i] + b->dieL[i]; outR[i] = b->dryR[i] + b->dieR[i]; }
    if (state != 2) goto done;

    // reverb
    wetTarget = (on && s->has_env && !s->env_paused) ? s->reverb / 100.0f : 0.0f;
    {
        int any = 0;
        float ramp = (float)SR_BLOCK / (0.15f * SR_RATE);      // 150 ms input crossfade
        memset(ambSum, 0, sizeof ambSum);
        // Paused, switched off or reverb=0, and already faded out: the tails
        // are drained silently, five blocks per block, so nothing is left
        // to come back on resume. (Reflection effects are never reset: a
        // reset effect loses the IR it took over and stays silent.)
        int draining = wetGain == 0 && (s->env_paused || !on || s->reverb == 0);
        for (r = 0; r < NROOM; r++) {
            float target = (wetTarget > 0 && s->env_room == r) ? 1.0f : 0.0f, f0 = room[r].feed, f1;
            IPLAudioBuffer bi, bo; float *pi[1], *po[4]; IPLAudioEffectState es; int pass, passes;
            if (!room[r].ready) continue;
            f1 = f0 < target ? (f0 + ramp > target ? target : f0 + ramp) : (f0 - ramp < target ? target : f0 - ramp);
            if (draining) f1 = f0 = 0;
            room[r].feed = f1;
            if (f0 == 0 && f1 == 0 && !room[r].active) continue;
            for (i = 0; i < SR_BLOCK; i++) tmpIn[i] = b->send[i] * roomGain[r] * (f0 + (f1 - f0) * (i + 1) / SR_BLOCK);
            pi[0] = tmpIn; for (k = 0; k < 4; k++) po[k] = amb[k];
            bi.numChannels = 1; bi.numSamples = SR_BLOCK; bi.data = pi;
            bo.numChannels = 4; bo.numSamples = SR_BLOCK; bo.data = po;
            passes = draining ? 5 : 1;
            for (pass = 0; pass < passes && (pass == 0 || room[r].active); pass++) {
                if (pass == 1) memset(tmpIn, 0, sizeof tmpIn);
                es = iplReflectionEffectApply(room[r].eff, &room[r].params, &bi, &bo, NULL);
                if (f1 > 0) { room[r].active = 1; room[r].quiet = 0; }
                else {
                    // The parametric part never reports its tail complete: stop
                    // once the output has stayed below -96 dB for 100 ms.
                    float pk = 0;
                    for (k = 0; k < 4; k++) for (i = 0; i < SR_BLOCK; i++) { float a = fabsf(amb[k][i]); if (a > pk) pk = a; }
                    room[r].quiet = pk < 1.6e-5f ? room[r].quiet + 1 : 0;
                    if (es == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE || room[r].quiet >= 10) { room[r].active = 0; room[r].quiet = 0; }
                }
            }
            if (draining) continue;
            for (k = 0; k < 4; k++) for (i = 0; i < SR_BLOCK; i++) ambSum[k][i] += amb[k][i];
            any = 1;
        }

        // master wet level: 100 ms glide, 60 ms fade on pause
        {
            float g0 = wetGain, g1, rate = (float)SR_BLOCK / ((s->env_paused ? 0.06f : 0.1f) * SR_RATE);
            // after a race (no live section) the tail keeps its level and dies naturally
            float tgt = wetTarget > 0 ? wetTarget : (s->env_paused || !on || s->reverb == 0) ? 0.0f : wetGain;
            g1 = g0 < tgt ? (g0 + rate > tgt ? tgt : g0 + rate) : (g0 - rate < tgt ? tgt : g0 - rate);
            wetGain = g1;
            if (any && (g0 > 0 || g1 > 0)) {
                // Always the speaker-panning decode: Steam Audio's binaural
                // decode of this first-order reverb measured +4.5 dB below
                // 160 Hz and -13 dB at 1.3-2.5 kHz, a dark, boomy tail.
                IPLAudioBuffer ba, bo; float *pa[4], *po[2]; IPLAmbisonicsDecodeEffectParams dp;
                for (k = 0; k < 4; k++) pa[k] = ambSum[k];
                ba.numChannels = 4; ba.numSamples = SR_BLOCK; ba.data = pa;
                bo.numChannels = 2; bo.numSamples = SR_BLOCK;
                memset(&dp, 0, sizeof dp);
                dp.order = 1; dp.hrtf = hrtf; dp.orientation = listener_space(); dp.binaural = IPL_FALSE;
                po[0] = wetL; po[1] = wetR; bo.data = po;
                iplAmbisonicsDecodeEffectApply(decPan, &dp, &ba, &bo);
                shelf_toward(&shelfWet, -10.0f * w);
                shelf_run(&shelfWet, 0, wetL);
                shelf_run(&shelfWet, 1, wetR);
                for (i = 0; i < SR_BLOCK; i++) {
                    float g = g0 + (g1 - g0) * (i + 1) / SR_BLOCK;
                    outL[i] += wetL[i] * g;
                    outR[i] += wetR[i] * g;
                }
            }
            // paused (or switched off) and faded out: drop the tails so nothing comes back on resume
            if (g1 == 0 && g0 > 0) {
                iplAmbisonicsDecodeEffectReset(decPan);
                memset(shelfWet.z1, 0, sizeof shelfWet.z1); memset(shelfWet.z2, 0, sizeof shelfWet.z2);
            }
        }
    }
    // With reverb running the sum can pass full scale: a soft knee above
    // -1 dBFS instead of the hard clip DirectSound would apply.
    if (wetGain > 0) {
        for (i = 0; i < SR_BLOCK; i++) { outL[i] = soft_clip(outL[i]); outR[i] = soft_clip(outR[i]); }
    }
done:
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    {
        LONGLONG us10 = (t1.QuadPart - t0.QuadPart) * 10000000 / f.QuadPart;   // 100 ns units
        fxTicks += us10; fxBlocks++;
        if (us10 > fxMax) fxMax = us10;
    }
}
