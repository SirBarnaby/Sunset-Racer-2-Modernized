// sr2fx: the shipped d3d9.dll of the Sunset Racer 2 build.
//
// The Shockwave 3D Xtra loads "D3D9.dll" by name, so this file beside the exe
// is the game's Direct3D 9. It forwards every call to the real renderer:
//   renderer=0  the system d3d9.dll (classic DirectX 9)
//   renderer=1  DXVK's d3d9.dll shipped renamed as sr2fx-vk.dll (Vulkan)
// and adds post effects (ambient occlusion, motion blur, sun shafts, post AA)
// at the end of the main camera's 3D pass, before any other camera, menu plane,
// HUD or text is drawn. With every setting 0 it only forwards:
// the game makes the same calls with the same parameters as without it.
//
// Settings: SunsetRacer2-fx.ini beside the exe, key=value lines written by the
// game's Lingo (see Settings in post.h). A worker thread polls the file's
// last-write time every 250 ms; the render thread never touches the file.
// Dev log: SunsetRacer2-d3d9.log beside the exe, only when SunsetRacer2-dev.txt
// exists.

#include "post.h"
#include "wrappers_gen.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------- basics

static char g_dir[MAX_PATH];
static FILE* g_log;
static CRITICAL_SECTION g_logCs;
static LARGE_INTEGER g_freq, g_t0;
bool g_dev;

static double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart;
}

void log_line(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    fprintf(g_log, "%9.3f ", now_s());
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static void init_once() {
    static bool done;
    if (done) return;
    done = true;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) *s = 0;
    char p[MAX_PATH];
    snprintf(p, MAX_PATH, "%s\\SunsetRacer2-dev.txt", g_dir);
    g_dev = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    if (g_dev) {
        snprintf(p, MAX_PATH, "%s\\SunsetRacer2-d3d9.log", g_dir);
        g_log = fopen(p, "w");
        log_line("sr2fx loaded, process %lu", GetCurrentProcessId());
    }
}

// ---------------------------------------------------------------- settings

static CRITICAL_SECTION g_setCs;
static Settings g_set;
static char g_iniPath[MAX_PATH];

static bool parse_ini(const char* b, size_t n, Settings& s) {
    memset(&s, 0, sizeof s);
    bool any = false;
    size_t i = 0;
    if (n >= 3 && (BYTE)b[0] == 0xEF && (BYTE)b[1] == 0xBB && (BYTE)b[2] == 0xBF) i = 3;
    while (i < n) {
        size_t e = i;
        while (e < n && b[e] != '\r' && b[e] != '\n') e++;
        // trim
        size_t a = i, z = e;
        while (a < z && (b[a] == ' ' || b[a] == '\t')) a++;
        while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t')) z--;
        if (z > a && b[a] != ';' && b[a] != '#') {
            const char* eq = (const char*)memchr(b + a, '=', z - a);
            if (!eq) return false;
            char key[32];
            size_t kl = eq - (b + a);
            while (kl && (b[a + kl - 1] == ' ' || b[a + kl - 1] == '\t')) kl--;
            if (kl == 0 || kl >= sizeof key) { i = e + 1; continue; }
            for (size_t k = 0; k < kl; k++) key[k] = (char)tolower((unsigned char)b[a + k]);
            key[kl] = 0;
            static const char* const known[] = {"renderer", "aa", "ao", "motionblur", "blurstrength", "sunshafts",
                                                "lightshadows", "env_paused", "env_speed", "fxdebug"};
            bool isKnown = false;
            for (const char* k : known) if (!strcmp(key, k)) isKnown = true;
            if (!isKnown) { i = e + 1; continue; }   // other modules' keys (audio, env_room, ...)
            const char* v = eq + 1;
            while (v < b + z && (*v == ' ' || *v == '\t')) v++;
            char num[24];
            size_t vl = (b + z) - v;
            if (vl == 0 || vl >= sizeof num) return false;
            memcpy(num, v, vl);
            num[vl] = 0;
            char* endp;
            long x = strtol(num, &endp, 10);
            if (*endp) return false;
            auto clampi = [](long v, long lo, long hi) { return (int)(v < lo ? lo : v > hi ? hi : v); };
            any = true;
            if (!strcmp(key, "renderer")) s.renderer = clampi(x, 0, 1);
            else if (!strcmp(key, "aa")) s.aa = clampi(x, 0, 2);
            else if (!strcmp(key, "ao")) s.ao = clampi(x, 0, 2);
            else if (!strcmp(key, "motionblur")) s.motionblur = clampi(x, 0, 2);
            else if (!strcmp(key, "blurstrength")) s.blurstrength = clampi(x, 0, 100);
            else if (!strcmp(key, "sunshafts")) s.sunshafts = clampi(x, 0, 1);
            else if (!strcmp(key, "lightshadows")) s.lightshadows = clampi(x, 0, 1);
            else if (!strcmp(key, "env_paused")) s.env_paused = clampi(x, 0, 1);
            else if (!strcmp(key, "env_speed")) s.env_speed = clampi(x, 0, 10000);
            else if (!strcmp(key, "fxdebug") && g_dev) s.fxdebug = clampi(x, 0, 3);
            // other keys (audio, env_room, env_wet) belong to other modules
        }
        i = e + 1;
    }
    return any || n == 0;
}

static FILETIME g_iniTime;
static DWORD g_iniSize = 0xFFFFFFFF;
static bool g_iniSeen;

// Reads the file when it changed. Called from the worker, and once from the
// render thread at device creation (the renderer choice needs it now).
static void poll_settings() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fa)) {
        if (g_iniSeen) {
            g_iniSeen = false;
            EnterCriticalSection(&g_setCs);
            memset(&g_set, 0, sizeof g_set);
            LeaveCriticalSection(&g_setCs);
            log_line("settings: file gone, all off");
        }
        return;
    }
    if (g_iniSeen && !memcmp(&fa.ftLastWriteTime, &g_iniTime, sizeof g_iniTime) && fa.nFileSizeLow == g_iniSize) return;
    Settings s = {};
    bool ok = false;
    // The game rewrites the whole file; a read can land mid-write. Retry a
    // few times before treating it as broken (= all off).
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        if (attempt) Sleep(30);
        HANDLE h = CreateFileA(g_iniPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        char buf[4096];
        DWORD got = 0;
        BOOL r = ReadFile(h, buf, sizeof buf, &got, nullptr);
        CloseHandle(h);
        if (!r || got == 0 || got == sizeof buf) continue;
        ok = parse_ini(buf, got, s);
    }
    if (!ok) memset(&s, 0, sizeof s);
    g_iniTime = fa.ftLastWriteTime;
    g_iniSize = fa.nFileSizeLow;
    g_iniSeen = true;
    EnterCriticalSection(&g_setCs);
    // The race rewrites the live keys (speed, paused) several times a second:
    // only a change of a real setting is worth a log line.
    Settings a = g_set, b = s;
    a.env_speed = b.env_speed = 0;
    a.env_paused = b.env_paused = 0;
    bool changed = memcmp(&a, &b, sizeof a) != 0;
    g_set = s;
    LeaveCriticalSection(&g_setCs);
    if (changed)
        log_line("settings%s: renderer %d aa %d ao %d motionblur %d strength %d sunshafts %d | paused %d speed %d",
                 ok ? "" : " (unreadable, all off)", s.renderer, s.aa, s.ao, s.motionblur, s.blurstrength, s.sunshafts,
                 s.env_paused, s.env_speed);
}

static CRITICAL_SECTION g_pollCs;

// Dev only: SunsetRacer2-d3d9-shot.txt (one line: an output .bmp path) makes the
// next presented frame, effects and HUD included, go to that file.
static char g_shotPath[MAX_PATH];
static volatile LONG g_shotPending;

static void poll_shot_request() {
    if (!g_dev || g_shotPending) return;
    char p[MAX_PATH];
    snprintf(p, MAX_PATH, "%s\\SunsetRacer2-d3d9-shot.txt", g_dir);
    FILE* f = fopen(p, "rb");
    if (!f) return;
    char line[MAX_PATH] = {0};
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) snprintf(line, MAX_PATH, "%s\\sr2fx-shot.bmp", g_dir);
    DeleteFileA(p);
    strcpy_s(g_shotPath, line);
    InterlockedExchange(&g_shotPending, 1);
}

// Dev only: while SunsetRacer2-d3d9-frames.txt exists, every presented frame's
// time since the previous one (ms) goes to SunsetRacer2-d3d9-frames.log, for
// frame-time percentiles without the cost of the game's own trace file.
static volatile LONG g_frameLogOn;
static FILE* g_frameLog;

static void poll_frame_log() {
    if (!g_dev) return;
    char p[MAX_PATH];
    snprintf(p, MAX_PATH, "%s\\SunsetRacer2-d3d9-frames.txt", g_dir);
    InterlockedExchange(&g_frameLogOn, GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
}

static void frame_log(double dtMs) {
    static bool was;
    bool on = g_frameLogOn != 0;
    if (on && !was) {
        char p[MAX_PATH];
        snprintf(p, MAX_PATH, "%s\\SunsetRacer2-d3d9-frames.log", g_dir);
        g_frameLog = fopen(p, "w");
    } else if (!on && was && g_frameLog) {
        fclose(g_frameLog);
        g_frameLog = nullptr;
    }
    was = on;
    if (on && g_frameLog) fprintf(g_frameLog, "%.3f\n", dtMs);
}

// Dev only: SunsetRacer2-d3d9-ftrace.txt ("start interval", seconds since the
// layer loaded) logs the full call sequence of one frame at start and then
// every interval seconds: cameras, viewports, clears, draws and where the
// effects went in. For checking the main-pass detection in any scene.
static volatile LONG g_traceReq;
static double g_traceNext = -1, g_traceEvery = 0;

static void poll_trace_request() {
    if (!g_dev) return;
    if (g_traceNext < 0) {
        char p[MAX_PATH];
        snprintf(p, MAX_PATH, "%s\\SunsetRacer2-d3d9-ftrace.txt", g_dir);
        FILE* f = fopen(p, "rb");
        if (!f) return;
        double a = 0, b = 0;
        if (fscanf(f, "%lf %lf", &a, &b) < 1) a = 0;
        fclose(f);
        g_traceNext = a;
        g_traceEvery = b;
    }
    if (g_traceNext >= 0 && now_s() >= g_traceNext && !g_traceReq) {
        InterlockedExchange(&g_traceReq, 1);
        g_traceNext = g_traceEvery > 0 ? now_s() + g_traceEvery : 1e18;
    }
}

static DWORD WINAPI settings_thread(LPVOID) {
    for (;;) {
        Sleep(250);
        EnterCriticalSection(&g_pollCs);
        poll_settings();
        LeaveCriticalSection(&g_pollCs);
        poll_shot_request();
        poll_frame_log();
        poll_trace_request();
    }
}

static void save_bmp(const char* path, const D3DLOCKED_RECT& lr, UINT w, UINT h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    UINT row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    std::vector<BYTE> buf(row);
    for (UINT y = 0; y < h; y++) {
        const BYTE* src = (const BYTE*)lr.pBits + (size_t)(h - 1 - y) * lr.Pitch;
        for (UINT x = 0; x < w; x++) { buf[x * 3] = src[x * 4]; buf[x * 3 + 1] = src[x * 4 + 1]; buf[x * 3 + 2] = src[x * 4 + 2]; }
        fwrite(buf.data(), 1, row, f);
    }
    fclose(f);
}

static Settings get_settings() {
    EnterCriticalSection(&g_setCs);
    Settings s = g_set;
    LeaveCriticalSection(&g_setCs);
    return s;
}

static void start_settings() {
    static bool started;
    if (started) return;
    started = true;
    snprintf(g_iniPath, MAX_PATH, "%s\\SunsetRacer2-fx.ini", g_dir);
    EnterCriticalSection(&g_pollCs);
    poll_settings();
    LeaveCriticalSection(&g_pollCs);
    HANDLE t = CreateThread(nullptr, 0, settings_thread, nullptr, 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
}

static Settings settings_now() {
    EnterCriticalSection(&g_pollCs);
    poll_settings();
    LeaveCriticalSection(&g_pollCs);
    return get_settings();
}

// ---------------------------------------------------------------- back ends

typedef IDirect3D9*(WINAPI* PFN_Create9)(UINT);
typedef HRESULT(WINAPI* PFN_Create9Ex)(UINT, IDirect3D9Ex**);
static HMODULE g_sys, g_vk;
static bool g_vkTried;

static HMODULE system_d3d9() {
    if (!g_sys) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat_s(p, "\\d3d9.dll");
        g_sys = LoadLibraryA(p);
        log_line("system d3d9: %s -> %p", p, g_sys);
    }
    return g_sys;
}

static void set_env_default(const char* k, const char* v) {
    char tmp[8];
    if (GetEnvironmentVariableA(k, tmp, sizeof tmp) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) SetEnvironmentVariableA(k, v);
}

static HMODULE vulkan_d3d9() {
    if (!g_vkTried) {
        g_vkTried = true;
        char p[MAX_PATH];
        snprintf(p, MAX_PATH, "%s\\sr2fx-vk.dll", g_dir);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
            log_line("vulkan: %s missing", p);
            return nullptr;
        }
        // Prefer the dedicated GPU on hybrid laptops; keep DXVK quiet unless
        // the dev file is there. Variables the player set win.
        set_env_default("DXVK_CONFIG", "dxvk.hideIntegratedGraphics = True");
        if (g_dev) {
            set_env_default("DXVK_LOG_LEVEL", "info");
            set_env_default("DXVK_LOG_PATH", g_dir);
        } else {
            set_env_default("DXVK_LOG_LEVEL", "none");
        }
        g_vk = LoadLibraryExA(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        log_line("vulkan: %s -> %p (error %lu)", p, g_vk, g_vk ? 0 : GetLastError());
    }
    return g_vk;
}

// DXVK answers Direct3DCreate9 even when no GPU can run it (no Vulkan driver,
// a GPU without Vulkan 1.3, a remote desktop): it then reports no adapters,
// and the Shockwave 3D Xtra crashes on the adapter it expects. A broken driver
// can also fault inside DXVK. Both count as "no Vulkan" and the game gets
// DirectX 9 instead. The checks run under SEH, away from C++ unwinding.
static IDirect3D9* vk_create_guarded(PFN_Create9 fn, UINT sdk, DWORD* code) {
    __try {
        return fn(sdk);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static const char* vk_problem(IDirect3D9* d) {
    if (d->GetAdapterCount() == 0) return "no adapter it can use";
    D3DCAPS9 caps;
    if (FAILED(d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) return "no device caps";
    D3DDISPLAYMODE mode;
    if (FAILED(d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) return "no display mode";
    return nullptr;
}

static const char* vk_problem_guarded(IDirect3D9* d, DWORD* code) {
    __try {
        return vk_problem(d);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return "fault while checking the adapter";
    }
}

// The Vulkan interface, or null when Vulkan cannot run here (then DirectX 9).
static IDirect3D9* vulkan_create(UINT sdk) {
    HMODULE m = vulkan_d3d9();
    auto fn = m ? (PFN_Create9)GetProcAddress(m, "Direct3DCreate9") : nullptr;
    if (!fn) return nullptr;
    DWORD code = 0;
    IDirect3D9* r = vk_create_guarded(fn, sdk, &code);
    if (!r) {
        log_line("vulkan: Direct3DCreate9 failed (exception 0x%lx), using DirectX 9", code);
        return nullptr;
    }
    if (const char* why = vk_problem_guarded(r, &code)) {
        log_line("vulkan: %s (exception 0x%lx), using DirectX 9", why, code);
        // A faulted instance is not released: its state is unknown.
        if (!code) r->Release();
        return nullptr;
    }
    return r;
}

static FARPROC sys_proc(const char* name) {
    HMODULE m = system_d3d9();
    return m ? GetProcAddress(m, name) : nullptr;
}

// ---------------------------------------------------------------- helpers

static const char* backend_name(bool vk) { return vk ? "Vulkan (DXVK)" : "DirectX 9"; }

static void log_pp(const char* what, const D3DPRESENT_PARAMETERS* p) {
    if (!p) return;
    log_line("%s: %ux%u fmt %d msaa %d/%lu swap %d windowed %d autodepth %d fmt %d flags 0x%lx interval 0x%x", what,
             p->BackBufferWidth, p->BackBufferHeight, p->BackBufferFormat, p->MultiSampleType, p->MultiSampleQuality,
             p->SwapEffect, p->Windowed, p->EnableAutoDepthStencil, p->AutoDepthStencilFormat, p->Flags,
             p->PresentationInterval);
}

// ---------------------------------------------------------------- IDirect3D9

class ProxyD3D9 : public ProxyD3D9Base {
public:
    ProxyD3D9(IDirect3D9* r, bool vk, UINT sdk) : m_vk(vk), m_sdk(sdk) { m_real = r; }
    ULONG m_ref = 1;
    bool m_vk;
    UINT m_sdk;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) { *pp = this; AddRef(); return S_OK; }
        return m_real->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { m_real->AddRef(); return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        m_real->Release();
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override;
};

// ---------------------------------------------------------------- device

// A moving object of the main scene, found again next frame for its velocity.
// Shockwave draws from its own system-memory arrays (DrawIndexedPrimitiveUP),
// and a freed array's address can come back for another model: the key also
// holds a fingerprint of the mesh (first and last vertex, first indices), so
// an address reused by different geometry never inherits its old motion.
struct ObjKey {
    const void* v; const void* i; UINT nv, prims, occ, sig;
    bool operator==(const ObjKey& o) const {
        return v == o.v && i == o.i && nv == o.nv && prims == o.prims && occ == o.occ && sig == o.sig;
    }
};
struct ObjKeyHash {
    size_t operator()(const ObjKey& k) const {
        size_t h = (size_t)k.v * 2654435761u;
        h ^= (size_t)k.i + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= k.nv * 40503u + k.prims * 2246822519u + k.occ * 3266489917u + k.sig;
        return h;
    }
};
struct ObjState {
    D3DMATRIX world, prevChange;
    double changeTime, interval, lastSeen;
};

static UINT fnv(const void* p, size_t n, UINT h = 2166136261u) {
    const BYTE* b = (const BYTE*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static double row_len(const D3DMATRIX& m, int r) {
    return sqrt((double)m.m[r][0] * m.m[r][0] + (double)m.m[r][1] * m.m[r][1] + (double)m.m[r][2] * m.m[r][2]);
}

// The same camera lens: Shockwave sets one projection per camera, so a draw
// with a different one belongs to another camera (menu planes, HUD models).
static bool same_lens(const D3DMATRIX& a, const D3DMATRIX& b) {
    return a._11 == b._11 && a._22 == b._22 && a._33 == b._33 && a._43 == b._43 && a._31 == b._31 && a._32 == b._32 &&
           a._34 == b._34 && a._44 == b._44;
}

static const int MIN_MAIN_DRAWS = 80;   // race main scene: 330-590 draws; menus: under 60
static const size_t MAX_OBJ_VERTS = 400000;

class ProxyDevice : public ProxyDeviceBase {
public:
    ProxyDevice(IDirect3DDevice9* r, ProxyD3D9* d3d, const D3DPRESENT_PARAMETERS& used) : m_d3d(d3d) {
        m_real = r;
        m_d3d->AddRef();
        m_bbW = used.BackBufferWidth;
        m_bbH = used.BackBufferHeight;
        memset(&m_view, 0, sizeof m_view);
        memset(&m_proj, 0, sizeof m_proj);
        memset(&m_world, 0, sizeof m_world);
        m_fogEnd = 1.0f;
    }
    ~ProxyDevice() { delete m_post; }

    ProxyD3D9* m_d3d;
    ULONG m_ref = 1;
    Post* m_post = nullptr;
    bool m_postDisabled = false;
    UINT m_bbW, m_bbH;

    // tracked state
    D3DMATRIX m_view, m_proj, m_world;
    D3DLIGHT9 m_light0 = {};
    bool m_haveLight0 = false;
    float m_fogStart = 0, m_fogEnd = 1;
    bool m_fogOn = false, m_zEnable = true, m_zWrite = true, m_blend = false;
    bool m_vpFull = true;
    D3DVIEWPORT9 m_vp = {};
    DWORD m_fvf = 0;
    bool m_vs = false;

    // frame
    bool m_inFrame = false, m_inserted = false, m_frameHadMain = false, m_prevHadMain = false;
    int m_scenePersp = 0;       // draws by the main camera candidate this scene
    bool m_sceneHasVP = false;
    D3DMATRIX m_sceneView, m_sceneProj;
    D3DLIGHT9 m_sceneLight = {};
    bool m_sceneHaveLight = false;
    float m_sceneFogStart = 0, m_sceneFogEnd = 1;
    bool m_sceneFogOn = false;
    Settings m_fs = {};
    double m_frameTime = 0, m_lastFrameTime = 0, m_dt = 1.0 / 60;
    bool m_tracking = false;

    // motion
    FrameData m_fd;
    D3DMATRIX m_prevView, m_prevProj;
    bool m_havePrevCam = false;
    double m_prevCamTime = 0;
    std::unordered_map<ObjKey, ObjState, ObjKeyHash> m_objs;
    std::unordered_map<ObjKey, UINT, ObjKeyHash> m_occ;
    double m_lastPurge = 0;

    // dev stats
    int m_stFrames = 0, m_stMain = 0, m_stRedirect = 0, m_stObjs = 0;
    double m_stStart = 0, m_stMaxDt = 0, m_stSumDt = 0;

    // dev frame trace (SunsetRacer2-d3d9-ftrace.txt)
    bool m_trace = false;
    char m_trSig[160] = {};
    int m_trCount = 0;
    int m_trObjs = 0;

    static bool translated(const D3DMATRIX& v) { return fabs(v._41) + fabs(v._42) + fabs(v._43) > 1.0f; }
    bool persp() const { return m_proj._34 != 0.0f; }
    bool main_running() const { return !m_inserted && m_scenePersp >= MIN_MAIN_DRAWS; }

    // ---- trace helpers (dev only, one frame at a time)
    void tr_flush() {
        if (m_trCount) log_line("  %4d x %s", m_trCount, m_trSig);
        m_trCount = 0;
        m_trSig[0] = 0;
    }
    void tr_event(const char* fmt, ...) {
        if (!m_trace) return;
        tr_flush();
        char b[300];
        va_list a;
        va_start(a, fmt);
        vsnprintf(b, sizeof b, fmt, a);
        va_end(a);
        log_line("  %s", b);
    }
    void tr_draw(const char* kind) {
        if (!m_trace) return;
        char s[160];
        snprintf(s, sizeof s, "%s %s%s vp%s z%d%d blend%d fvf 0x%lx%s%s", kind, persp() ? "persp" : "ortho",
                 m_sceneHasVP && same_lens(m_proj, m_sceneProj) ? "(main lens)" : "", m_vpFull ? "full" : "part", m_zEnable,
                 m_zWrite, m_blend, m_fvf, m_vs ? " vs" : "", translated(m_view) ? "" : " view@origin");
        if (strcmp(s, m_trSig)) { tr_flush(); strcpy_s(m_trSig, s); }
        m_trCount++;
    }

    void disable_post(const char* why) {
        if (m_post) { m_post->end_redirect(); delete m_post; m_post = nullptr; }
        if (!m_postDisabled) log_line("effects off for this device: %s", why);
        m_postDisabled = true;
    }

    void begin_frame() {
        m_inFrame = true;
        m_inserted = false;
        m_frameHadMain = false;
        m_frameTime = now_s();
        m_dt = m_lastFrameTime > 0 ? std::max(0.0005, std::min(0.25, m_frameTime - m_lastFrameTime)) : 1.0 / 60;
        m_lastFrameTime = m_frameTime;
        m_fs = get_settings();
        m_tracking = false;
        if (g_dev && g_traceReq) {
            InterlockedExchange(&g_traceReq, 0);
            m_trace = true;
            m_trObjs = 0;
            log_line("frame trace begins (back buffer %ux%u, previous frame had a main scene: %d)", m_bbW, m_bbH, m_prevHadMain);
        }
        // Menus and loading screens: nothing more to do. The layer costs a
        // settings copy per frame there.
        if (!m_prevHadMain || !wants_post(m_fs) || m_postDisabled) return;
        m_fd.objs.clear();
        m_fd.pos.clear();
        m_fd.idx.clear();
        m_occ.clear();
        // A lost device (lock screen, display change) draws nothing until the
        // game resets it: stay on the game's targets meanwhile.
        if (m_real->TestCooperativeLevel() != D3D_OK) return;
        if (!m_post) {
            m_post = new Post(m_real);
            if (!m_post->ok()) { disable_post("shaders"); return; }
        }
        if (m_post->begin_redirect(m_fs)) {
            m_stRedirect++;
            m_tracking = m_fs.motionblur != 0;
            tr_event("scene redirected to the layer's targets");
        } else if (!m_post->ok()) {
            disable_post("render targets");
        }
    }

    // The main camera finished its 3D: run the effects before anything else
    // (other cameras, menu planes, HUD, text) is drawn.
    void insert_post(const char* why) {
        tr_event(">>> main pass ends (%s) after %d main draws: effects %s", why, m_scenePersp,
                 m_post && m_post->redirected() ? "run here" : "not active");
        m_inserted = true;
        m_frameHadMain = true;
        m_stMain++;
        // Camera cut or a long gap: no previous camera for motion blur.
        if (m_havePrevCam) {
            double dx = m_sceneView._41 - m_prevView._41, dy = m_sceneView._42 - m_prevView._42, dz = m_sceneView._43 - m_prevView._43;
            if (dx * dx + dy * dy + dz * dz > 1500.0 * 1500.0 || m_frameTime - m_prevCamTime > 0.25) m_havePrevCam = false;
        }
        if (m_post && m_post->redirected()) {
            m_fd.view = m_sceneView;
            m_fd.proj = m_sceneProj;
            m_fd.prevView = m_havePrevCam ? m_prevView : m_sceneView;
            m_fd.prevProj = m_havePrevCam ? m_prevProj : m_sceneProj;
            m_fd.havePrev = m_havePrevCam;
            m_fd.sun = m_sceneLight;
            m_fd.haveSun = m_sceneHaveLight;
            m_fd.fogStart = m_sceneFogStart;
            m_fd.fogEnd = m_sceneFogEnd;
            m_fd.fogOn = m_sceneFogOn;
            m_fd.dt = m_dt;
            m_stObjs += (int)m_fd.objs.size();
            m_post->apply(m_fs, m_fd);
        }
        m_tracking = false;
        m_prevView = m_sceneView;
        m_prevProj = m_sceneProj;
        m_havePrevCam = true;
        m_prevCamTime = m_frameTime;
    }

    // Called before every draw and depth clear: a draw that the main camera
    // would not make (another lens, a partial viewport, orthographic) means
    // its pass is over.
    void before_draw(const char* kind) {
        if (main_running()) {
            const char* why = nullptr;
            if (!m_vpFull) why = "viewport changed";
            else if (!persp()) why = "orthographic draw";
            else if (!same_lens(m_proj, m_sceneProj)) why = "another camera";
            if (why) insert_post(why);
        }
        tr_draw(kind);
    }

    // A draw of the 3D kind a main camera makes: perspective, whole window,
    // camera away from the origin. The first such draw picks the lens; while
    // fewer than MIN_MAIN_DRAWS used it, another lens takes over.
    bool count_main_draw() {
        if (m_inserted || !persp() || !m_vpFull || !translated(m_view)) return false;
        if (!m_sceneHasVP || (!same_lens(m_proj, m_sceneProj) && m_scenePersp < MIN_MAIN_DRAWS)) {
            if (m_sceneHasVP && m_scenePersp) tr_event("candidate camera replaced after %d draws", m_scenePersp);
            m_sceneView = m_view;
            m_sceneProj = m_proj;
            m_sceneHasVP = true;
            m_scenePersp = 0;
        }
        if (!same_lens(m_proj, m_sceneProj)) return false;
        m_scenePersp++;
        m_sceneLight = m_light0;
        m_sceneHaveLight = m_haveLight0;
        m_sceneFogStart = m_fogStart;
        m_sceneFogEnd = m_fogEnd;
        m_sceneFogOn = m_fogOn;
        return true;
    }

    // The main scene's opaque draws are done (Shockwave draws every opaque
    // model first, then the see-through ones back to front, then particles):
    // AO goes in now so smoke, rain, glows and glass are not shaded by it.
    void early_ao() {
        if (!m_fs.ao || !main_running() || !m_post || !m_post->redirected() || m_post->ao_early_done()) return;
        tr_event("AO multiplied into the scene before the see-through draws");
        m_fd.view = m_sceneView;
        m_fd.proj = m_sceneProj;
        m_fd.fogStart = m_sceneFogStart;
        m_fd.fogEnd = m_sceneFogEnd;
        m_fd.fogOn = m_sceneFogOn;
        m_post->apply_ao_early(m_fs, m_fd);
    }

    void resolve_now(bool inScene) {
        if (!m_post || !m_post->redirected()) return;
        tr_event("no main pass found: scene copied to the back buffer unchanged");
        if (!inScene) m_real->BeginScene();
        m_post->resolve();
        if (!inScene) m_real->EndScene();
    }

    void end_frame() {
        if (!m_inFrame) return;
        resolve_now(false);
        m_tracking = false;
        m_prevHadMain = m_frameHadMain;
        if (!m_frameHadMain) m_havePrevCam = false;
        m_inFrame = false;
        if (m_trace) {
            tr_flush();
            log_line("frame trace ends");
            m_trace = false;
        }
        if (!m_objs.empty() && m_frameTime - m_lastPurge > 2.0) {
            m_lastPurge = m_frameTime;
            for (auto it = m_objs.begin(); it != m_objs.end();)
                it = (m_frameTime - it->second.lastSeen > 2.0) ? m_objs.erase(it) : std::next(it);
        }
        if (g_dev) {
            m_stFrames++;
            m_stSumDt += m_dt;
            if (m_dt > m_stMaxDt) m_stMaxDt = m_dt;
            if (m_frameTime - m_stStart >= 1.0) {
                if (m_stStart > 0)
                    log_line("frames %d (%.1f fps, avg %.2f ms, max %.1f ms) main %d redirected %d moving objects/frame %.1f | %s",
                             m_stFrames, m_stFrames / (m_frameTime - m_stStart), 1000 * m_stSumDt / m_stFrames,
                             1000 * m_stMaxDt, m_stMain, m_stRedirect, m_stMain ? (double)m_stObjs / m_stMain : 0.0,
                             backend_name(m_d3d->m_vk));
                if (m_post) m_post->log_timings();
                m_stStart = m_frameTime;
                m_stFrames = m_stMain = m_stRedirect = m_stObjs = 0;
                m_stMaxDt = m_stSumDt = 0;
            }
        }
    }

    void track_object(D3DPRIMITIVETYPE type, UINT minv, UINT nv, UINT prims, const void* idx, D3DFORMAT ifmt,
                      const void* vtx, UINT stride) {
        if (!vtx || !idx || stride < 12 || nv == 0 || nv > 65535 || m_vs) return;
        if ((m_fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return;
        UINT nidx;
        switch (type) {
        case D3DPT_TRIANGLELIST: nidx = prims * 3; break;
        case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: nidx = prims + 2; break;
        default: return;
        }
        const BYTE* v0 = (const BYTE*)vtx + (size_t)minv * stride;
        UINT sig = fnv(v0, 12);
        sig = fnv(v0 + (size_t)(nv - 1) * stride, 12, sig);
        sig = fnv(idx, (ifmt == D3DFMT_INDEX32 ? 4 : 2) * std::min(nidx, 3u), sig);
        ObjKey base = {vtx, idx, nv, prims, 0, sig};
        UINT occ = m_occ[base]++;
        ObjKey key = base;
        key.occ = occ;
        double now = m_frameTime;
        auto it = m_objs.find(key);
        if (it == m_objs.end()) {
            ObjState st;
            st.world = st.prevChange = m_world;
            st.changeTime = -1e9;
            st.interval = 1.0 / 30;
            st.lastSeen = now;
            m_objs.emplace(key, st);
            return;
        }
        ObjState& st = it->second;
        bool gap = now - st.lastSeen > 0.25;
        st.lastSeen = now;
        if (memcmp(&st.world, &m_world, sizeof m_world)) {
            double dx = m_world._41 - st.world._41, dy = m_world._42 - st.world._42, dz = m_world._43 - st.world._43;
            // A jump, or a change of size: not the same object moving. Start over
            // without a velocity rather than smear it across the screen.
            bool rescaled = false;
            for (int r = 0; r < 3; r++) {
                double a = row_len(m_world, r), b = row_len(st.world, r);
                if (fabs(a - b) > 0.05 * std::max(a, b)) rescaled = true;
            }
            if (gap || rescaled || dx * dx + dy * dy + dz * dz > 3000.0 * 3000.0) {
                st.world = st.prevChange = m_world;
                st.changeTime = -1e9;
                return;
            }
            st.interval = std::max(m_dt, std::min(0.1, now - st.changeTime));
            st.prevChange = st.world;
            st.world = m_world;
            st.changeTime = now;
        }
        if (now - st.changeTime > std::max(2.5 * st.interval, 0.05)) return;   // not moving
        if (m_fd.pos.size() / 3 + nv > MAX_OBJ_VERTS) return;

        ObjDraw o;
        o.type = type;
        o.firstVertex = (UINT)(m_fd.pos.size() / 3);
        o.numVertices = nv;
        o.firstIndex = (UINT)m_fd.idx.size();
        o.primCount = prims;
        o.world = st.world;
        o.prevWorld = st.prevChange;
        o.share = (float)std::min(1.0, m_dt / st.interval);
        size_t p0 = m_fd.pos.size();
        m_fd.pos.resize(p0 + (size_t)nv * 3);
        const BYTE* src = v0;
        float* dst = &m_fd.pos[p0];
        for (UINT i = 0; i < nv; i++, src += stride, dst += 3) memcpy(dst, src, 12);
        if (m_trace && m_trObjs < 12) {
            // Size of the moving object in world units, and its distance: what
            // the AO radius and fade distances are tuned against.
            m_trObjs++;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (UINT i = 0; i < nv; i++)
                for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], m_fd.pos[p0 + i * 3 + k]); hi[k] = std::max(hi[k], m_fd.pos[p0 + i * 3 + k]); }
            double cx = m_world._41, cy = m_world._42, cz = m_world._43;
            double vz = cx * m_view._13 + cy * m_view._23 + cz * m_view._33 + m_view._43;
            tr_event("moving object: %u verts, size %.0f x %.0f x %.0f world units, view depth %.0f", nv,
                     (hi[0] - lo[0]) * row_len(m_world, 0), (hi[1] - lo[1]) * row_len(m_world, 1),
                     (hi[2] - lo[2]) * row_len(m_world, 2), vz);
        }
        size_t i0 = m_fd.idx.size();
        m_fd.idx.resize(i0 + nidx);
        WORD* di = &m_fd.idx[i0];
        if (ifmt == D3DFMT_INDEX32) {
            const DWORD* s = (const DWORD*)idx;
            for (UINT i = 0; i < nidx; i++) { DWORD v = s[i] - minv; di[i] = (WORD)(v < nv ? v : 0); }
        } else {
            const WORD* s = (const WORD*)idx;
            for (UINT i = 0; i < nidx; i++) { UINT v = (UINT)s[i] - minv; di[i] = (WORD)(v < nv ? v : 0); }
        }
        m_fd.objs.push_back(o);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) { *pp = this; AddRef(); return S_OK; }
        return m_real->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { m_real->AddRef(); return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        if (m_ref == 1) {
            // The layer's own resources hold device references: free them first.
            delete m_post;
            m_post = nullptr;
            ULONG r = m_real->Release();
            log_line("device released (real refcount %lu)", r);
            m_d3d->Release();
            m_ref = 0;
            delete this;
            return 0;
        }
        m_real->Release();
        return --m_ref;
    }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** pp) override {
        if (!pp) return D3DERR_INVALIDCALL;
        *pp = m_d3d;
        m_d3d->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override {
        if (m_post) { m_post->end_redirect(); m_post->on_lost(); }
        m_inFrame = false;
        m_prevHadMain = false;
        m_havePrevCam = false;
        m_objs.clear();
        if (!pp) return m_real->Reset(pp);
        D3DPRESENT_PARAMETERS used = *pp;
        log_pp("Reset", pp);
        HRESULT hr = m_real->Reset(&used);
        log_line("Reset -> 0x%lx", hr);
        if (SUCCEEDED(hr)) {
            *pp = used;
            m_bbW = used.BackBufferWidth;
            m_bbH = used.BackBufferHeight;
            m_postDisabled = false;
            // Reset puts every render state back to its default.
            m_zEnable = used.EnableAutoDepthStencil != FALSE;
            m_zWrite = true;
            m_blend = false;
            m_fogOn = false;
            m_vpFull = true;
        }
        return hr;
    }

    void capture_frame() {
        IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
        D3DSURFACE_DESC d;
        if (FAILED(m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
        bb->GetDesc(&d);
        IDirect3DSurface9* src = bb;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
            SUCCEEDED(m_real->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
            SUCCEEDED(m_real->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE)))
            src = rt;
        if (SUCCEEDED(m_real->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(m_real->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                save_bmp(g_shotPath, lr, d.Width, d.Height);
                sys->UnlockRect();
                log_line("shot saved: %s (msaa %d)", g_shotPath, d.MultiSampleType);
            }
        }
        if (sys) sys->Release();
        if (rt) rt->Release();
        bb->Release();
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty) override {
        if (g_dev) {
            static double last;
            double t = now_s();
            if (last > 0) frame_log((t - last) * 1000.0);
            last = t;
        }
        end_frame();
        if (g_shotPending) { capture_frame(); InterlockedExchange(&g_shotPending, 0); }
        return m_real->Present(src, dst, hwnd, dirty);
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) override {
        // Direct to stage off: the frame is read back instead of presented.
        end_frame();
        return m_real->GetRenderTargetData(rt, dst);
    }

    HRESULT STDMETHODCALLTYPE BeginScene() override {
        HRESULT hr = m_real->BeginScene();
        if (!m_inFrame) begin_frame();
        tr_event("BeginScene");
        m_scenePersp = 0;
        m_sceneHasVP = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EndScene() override {
        if (main_running()) insert_post("end of scene");
        tr_event("EndScene");
        m_scenePersp = 0;
        return m_real->EndScene();
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st) override {
        // A depth clear starts another camera's pass.
        if ((flags & D3DCLEAR_ZBUFFER) && main_running()) insert_post("depth cleared for another camera");
        tr_event("Clear flags 0x%lx rects %lu%s", flags, n, m_vpFull ? "" : " (partial viewport)");
        return m_real->Clear(n, r, flags, c, z, st);
    }

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) override {
        if (m) {
            if (st == D3DTS_WORLD) m_world = *m;
            else if (st == D3DTS_VIEW) {
                if (m_trace && memcmp(&m_view, m, sizeof *m))
                    tr_event("view: camera at %.0f %.0f %.0f (view row 4)", m->_41, m->_42, m->_43);
                m_view = *m;
            } else if (st == D3DTS_PROJECTION) {
                // The first orthographic projection after the main scene's 3D
                // starts the HUD: the effects go in just before it.
                if (m->_34 == 0.0f && m->_44 != 0.0f && main_running()) insert_post("orthographic projection");
                if (m_trace && memcmp(&m_proj, m, sizeof *m))
                    tr_event("projection: %s %.4f %.4f %.6f %.3f", m->_34 != 0.0f ? "persp" : "ortho", m->_11, m->_22, m->_33, m->_43);
                m_proj = *m;
            }
        }
        return m_real->SetTransform(st, m);
    }

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9* s) override {
        tr_event("game SetRenderTarget %lu", i);
        if (m_post && m_post->redirected()) {
            if (main_running()) insert_post("render target changed");
            else resolve_now(true);
        }
        return m_real->SetRenderTarget(i, s);
    }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* s) override {
        tr_event("game SetDepthStencilSurface");
        if (m_post && m_post->redirected()) {
            if (main_running()) insert_post("depth buffer changed");
            else resolve_now(true);
        }
        return m_real->SetDepthStencilSurface(s);
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i, IDirect3DSurface9** pp) override {
        if (i == 0 && pp && m_post && m_post->redirected() && m_post->game_rt()) {
            *pp = m_post->game_rt();
            (*pp)->AddRef();
            return S_OK;
        }
        return m_real->GetRenderTarget(i, pp);
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** pp) override {
        if (pp && m_post && m_post->redirected()) {
            *pp = m_post->game_ds();
            if (!*pp) return D3DERR_NOTFOUND;
            (*pp)->AddRef();
            return S_OK;
        }
        return m_real->GetDepthStencilSurface(pp);
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override {
        switch (s) {
        case D3DRS_FOGSTART: memcpy(&m_fogStart, &v, 4); break;
        case D3DRS_FOGEND: memcpy(&m_fogEnd, &v, 4); break;
        case D3DRS_FOGENABLE: m_fogOn = v != 0; break;
        case D3DRS_ZENABLE: m_zEnable = v != 0; break;
        case D3DRS_ZWRITEENABLE: m_zWrite = v != 0; break;
        case D3DRS_ALPHABLENDENABLE: m_blend = v != 0; break;
        default: break;
        }
        return m_real->SetRenderState(s, v);
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* v) override {
        if (v) {
            m_vpFull = v->X == 0 && v->Y == 0 && v->Width == m_bbW && v->Height == m_bbH;
            if (m_trace && memcmp(&m_vp, v, sizeof *v))
                tr_event("viewport %lu,%lu %lux%lu z %.2f-%.2f", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
            m_vp = *v;
        }
        return m_real->SetViewport(v);
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* l) override {
        if (i == 0 && l) { m_light0 = *l; m_haveLight0 = true; }
        return m_real->SetLight(i, l);
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD f) override { m_fvf = f; return m_real->SetFVF(f); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* s) override { m_vs = s != nullptr; return m_real->SetVertexShader(s); }

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minv, UINT nv, UINT n, const void* i, D3DFORMAT f,
                                                     const void* d, UINT stride) override {
        before_draw("mesh");
        if (count_main_draw()) {
            if (m_blend) early_ao();
            // Blended draws (rain, glass, smoke) keep the velocity of what is behind them.
            if (m_tracking && m_zEnable && m_zWrite && !m_blend) track_object(t, minv, nv, n, i, f, d, stride);
        }
        return m_real->DrawIndexedPrimitiveUP(t, minv, nv, n, i, f, d, stride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT n) override {
        before_draw("buffer mesh");
        if (count_main_draw() && m_blend) early_ao();
        return m_real->DrawIndexedPrimitive(t, base, minv, nv, start, n);
    }
    // Particles (point sprites in view space) come after every model.
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT n) override {
        before_draw("buffer prims");
        early_ao();
        return m_real->DrawPrimitive(t, start, n);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT n, const void* d, UINT stride) override {
        before_draw("prims");
        early_ao();
        return m_real->DrawPrimitiveUP(t, n, d, stride);
    }
};

HRESULT STDMETHODCALLTYPE ProxyD3D9::CreateDevice(UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (!pp || !out) return m_real->CreateDevice(a, t, focus, flags, pp, out);
    // The game's own parameters, MSAA included: the effects draw the 3D scene
    // into the layer's single-sample targets, and the menus and HUD keep the
    // multisampled back buffer the player chose.
    D3DPRESENT_PARAMETERS used = *pp;
    D3DADAPTER_IDENTIFIER9 id = {};
    m_real->GetAdapterIdentifier(a, 0, &id);
    log_line("CreateDevice on %s, adapter %u '%s', flags 0x%lx", backend_name(m_vk), a, id.Description, flags);
    log_pp("  parameters", pp);
    HRESULT hr = m_real->CreateDevice(a, t, focus, flags, &used, out);
    if (FAILED(hr) && m_vk) {
        // Vulkan could not make this device: fall back to DirectX 9 for good.
        log_line("CreateDevice on Vulkan failed 0x%lx, falling back to DirectX 9", hr);
        auto fn = (PFN_Create9)sys_proc("Direct3DCreate9");
        IDirect3D9* sys = fn ? fn(m_sdk) : nullptr;
        if (sys) {
            for (ULONG i = 0; i < m_ref; i++) m_real->Release();
            m_real = sys;
            for (ULONG i = 1; i < m_ref; i++) m_real->AddRef();
            m_vk = false;
            used = *pp;
            hr = m_real->CreateDevice(a, t, focus, flags, &used, out);
        }
    }
    log_line("CreateDevice -> 0x%lx", hr);
    if (SUCCEEDED(hr) && *out) {
        *pp = used;
        *out = new ProxyDevice(*out, this, used);
    }
    return hr;
}

// ---------------------------------------------------------------- exports

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    init_once();
    start_settings();
    Settings s = settings_now();
    IDirect3D9* r = nullptr;
    bool vk = false;
    if (s.renderer == 1) {
        r = vulkan_create(sdk);
        vk = r != nullptr;
    }
    if (!r) {
        auto fn = (PFN_Create9)sys_proc("Direct3DCreate9");
        if (!fn) return nullptr;
        r = fn(sdk);
        if (!r) return nullptr;
    }
    log_line("Direct3DCreate9(%u): %s", sdk, backend_name(vk));
    return new ProxyD3D9(r, vk, sdk);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** pp) {
    init_once();
    auto fn = (PFN_Create9Ex)sys_proc("Direct3DCreate9Ex");
    return fn ? fn(sdk, pp) : D3DERR_NOTAVAILABLE;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) {
    auto fn = (int(WINAPI*)(D3DCOLOR, LPCWSTR))sys_proc("D3DPERF_BeginEvent");
    return fn ? fn(c, n) : 0;
}
int WINAPI D3DPERF_EndEvent() {
    auto fn = (int(WINAPI*)())sys_proc("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {
    auto fn = (void(WINAPI*)(D3DCOLOR, LPCWSTR))sys_proc("D3DPERF_SetMarker");
    if (fn) fn(c, n);
}
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {
    auto fn = (void(WINAPI*)(D3DCOLOR, LPCWSTR))sys_proc("D3DPERF_SetRegion");
    if (fn) fn(c, n);
}
BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    auto fn = (BOOL(WINAPI*)())sys_proc("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}
void WINAPI D3DPERF_SetOptions(DWORD o) {
    auto fn = (void(WINAPI*)(DWORD))sys_proc("D3DPERF_SetOptions");
    if (fn) fn(o);
}
DWORD WINAPI D3DPERF_GetStatus() {
    auto fn = (DWORD(WINAPI*)())sys_proc("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_setCs);
        InitializeCriticalSection(&g_pollCs);
        // Stay loaded for the life of the process. The Shockwave 3D Xtra loads
        // "D3D9.dll" to test for DirectX 9, frees it, and loads it again for
        // the device; freed in between, the system d3d9.dll this DLL loaded
        // would answer the second load under the same name.
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&DllMain, &self);
    }
    return TRUE;
}
