// Exports of the real dsound.dll, forwarded to System32, plus the one device
// hook (CreateSoundBuffer / DuplicateSoundBuffer).
#include "sr2audio.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int sr_dev;
static HMODULE self, real;
static CRITICAL_SECTION logcs;
static HANDLE logf = INVALID_HANDLE_VALUE;
static DWORD t0;

void sr_exe_dir_file(const char *name, char *out, int cap) {
    char *s;
    GetModuleFileNameA(NULL, out, cap);
    s = strrchr(out, '\\');
    if (s) s[1] = 0; else out[0] = 0;
    strncat(out, name, cap - strlen(out) - 1);
}
void sr_exe_dir_filew(const wchar_t *name, wchar_t *out, int cap) {
    wchar_t *s;
    GetModuleFileNameW(NULL, out, cap);
    s = wcsrchr(out, L'\\');
    if (s) s[1] = 0; else out[0] = 0;
    wcsncat(out, name, cap - wcslen(out) - 1);
}

void sr_log(const char *fmt, ...) {
    char buf[1024]; int n; DWORD w; va_list a;
    if (!sr_dev) return;
    EnterCriticalSection(&logcs);
    if (logf == INVALID_HANDLE_VALUE) {
        char p[MAX_PATH];
        sr_exe_dir_file("SunsetRacer2-audio.log", p, MAX_PATH);
        logf = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (logf != INVALID_HANDLE_VALUE) {
        n = _snprintf(buf, sizeof buf - 2, "%8.3f ", (GetTickCount() - t0) / 1000.0);
        va_start(a, fmt);
        n += _vsnprintf(buf + n, sizeof buf - 2 - n, fmt, a);
        va_end(a);
        if (n < 0 || n > (int)sizeof buf - 2) n = sizeof buf - 2;
        buf[n++] = '\r'; buf[n++] = '\n';
        WriteFile(logf, buf, n, &w, NULL);
    }
    LeaveCriticalSection(&logcs);
}

static void load_real(void) {
    char p[MAX_PATH];
    if (real) return;
    GetSystemDirectoryA(p, MAX_PATH);
    strcat(p, "\\dsound.dll");
    real = LoadLibraryA(p);
}

// ---- the device hook -------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_CSB)(IDirectSound *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DSB)(IDirectSound *, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER *);
typedef ULONG (STDMETHODCALLTYPE *PFN_REL)(IDirectSound *);

#define MAXVT 4
static struct { void **vt; PFN_CSB csb; PFN_DSB dup; PFN_REL rel; } vts[MAXVT];
static int nvts;
static CRITICAL_SECTION hookcs;
static volatile LONG created_real, created_virtual;

static int vt_index(IDirectSound *ds) {
    void **vt = *(void ***)ds; int i;
    for (i = 0; i < nvts; i++) if (vts[i].vt == vt) return i;
    return -1;
}

static int format_ok(LPCDSBUFFERDESC d) {
    const WAVEFORMATEX *w = d->lpwfxFormat;
    if (!w || (d->dwFlags & (DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLFX))) return 0;
    if (w->wFormatTag != WAVE_FORMAT_PCM) return 0;
    if (w->nChannels < 1 || w->nChannels > 2) return 0;
    if (w->wBitsPerSample != 8 && w->wBitsPerSample != 16) return 0;
    if (w->nBlockAlign != w->nChannels * w->wBitsPerSample / 8) return 0;
    if (w->nSamplesPerSec < 100 || w->nSamplesPerSec > 200000) return 0;
    if (d->dwBufferBytes < 4 || d->dwBufferBytes > 0x1000000) return 0;
    return 1;
}

static HRESULT STDMETHODCALLTYPE hCSB(IDirectSound *ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN u) {
    int i = vt_index(ds);
    PFN_CSB orig = i >= 0 ? vts[i].csb : NULL;
    if (!orig) return DSERR_GENERIC;
    if (d && out && !u && d->dwSize >= sizeof(DSBUFFERDESC1) && sr_steamaudio_on() && format_ok(d)) {
        // Until Steam Audio has loaded (it starts loading as soon as the
        // setting is on) new sounds stay on the original path.
        if (fx_init() && fx_ready()) {
            mixer_attach_device(ds, (void *)orig);
            HRESULT hr = vbuf_create(d, out);
            if (SUCCEEDED(hr)) { InterlockedIncrement(&created_virtual); return hr; }
        }
    }
    {
        LONG n = InterlockedIncrement(&created_real);
        if (sr_dev && d && (sr_steamaudio_on() || n <= 3 || n % 50 == 0))
            sr_log("buffer %ld on the original DirectSound path (flags=%lx, mode %d, format ok %d, Steam Audio ready %d)",
                   n, d->dwFlags, sr_steamaudio_on(), format_ok(d), fx_ready());
    }
    return orig(ds, d, out, u);
}

static HRESULT STDMETHODCALLTYPE hDup(IDirectSound *ds, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER *out) {
    int i = vt_index(ds);
    if (src && out && vbuf_is(src)) return vbuf_duplicate((VBuf *)src, out);
    return i >= 0 ? vts[i].dup(ds, src, out) : DSERR_GENERIC;
}

static void release_real(IDirectSound *ds) { int i = vt_index(ds); if (i >= 0) vts[i].rel(ds); }

// The mixer holds one reference to the device it plays on (and the output
// buffer it made on it). When the game drops its own last reference, the
// mixer is stopped and lets go too, so the device goes away as it would.
static ULONG STDMETHODCALLTYPE hRel(IDirectSound *ds) {
    int i = vt_index(ds);
    ULONG r;
    if (i < 0) return 0;
    if (ds != mixer_device()) return vts[i].rel(ds);
    r = vts[i].rel(ds);
    if (r == 1) {
        mixer_shutdown(release_real);
        return 0;
    }
    return r - 1;
}

static void patch(void **slot, void *fn) {
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static void hook_device(IDirectSound *ds) {
    void **vt = *(void ***)ds;
    EnterCriticalSection(&hookcs);
    if (vt_index(ds) < 0 && nvts < MAXVT) {
        vts[nvts].vt = vt;
        vts[nvts].csb = (PFN_CSB)vt[3];
        vts[nvts].dup = (PFN_DSB)vt[5];
        vts[nvts].rel = (PFN_REL)vt[2];
        nvts++;
        // A vtable shared by several classes would already point at us.
        if (vt[3] != (void *)hCSB) patch(&vt[3], (void *)hCSB);
        if (vt[5] != (void *)hDup) patch(&vt[5], (void *)hDup);
        if (vt[2] != (void *)hRel) patch(&vt[2], (void *)hRel);
    }
    LeaveCriticalSection(&hookcs);
}

static void first_use(void) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    sr_settings_start();
    if (sr_steamaudio_on()) fx_init();
}

typedef HRESULT (WINAPI *PFN_Create)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_Create8)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);

HRESULT WINAPI P_DirectSoundCreate(LPCGUID g, LPDIRECTSOUND *ds, LPUNKNOWN u) {
    HRESULT hr;
    load_real(); first_use();
    hr = ((PFN_Create)GetProcAddress(real, "DirectSoundCreate"))(g, ds, u);
    if (SUCCEEDED(hr) && ds && *ds) hook_device(*ds);
    sr_log("DirectSoundCreate hr=%08lx", hr);
    return hr;
}
HRESULT WINAPI P_DirectSoundCreate8(LPCGUID g, LPDIRECTSOUND8 *ds, LPUNKNOWN u) {
    HRESULT hr;
    load_real(); first_use();
    hr = ((PFN_Create8)GetProcAddress(real, "DirectSoundCreate8"))(g, ds, u);
    if (SUCCEEDED(hr) && ds && *ds) hook_device((IDirectSound *)*ds);
    sr_log("DirectSoundCreate8 hr=%08lx", hr);
    return hr;
}

#define FWD(name, sig, args) typedef HRESULT (WINAPI *T_##name) sig; \
    HRESULT WINAPI P_##name sig { load_real(); return ((T_##name)GetProcAddress(real, #name)) args; }
FWD(DirectSoundEnumerateA, (LPDSENUMCALLBACKA c, LPVOID x), (c, x))
FWD(DirectSoundEnumerateW, (LPDSENUMCALLBACKW c, LPVOID x), (c, x))
FWD(DirectSoundCaptureCreate, (LPCGUID g, LPDIRECTSOUNDCAPTURE *c, LPUNKNOWN u), (g, c, u))
FWD(DirectSoundCaptureEnumerateA, (LPDSENUMCALLBACKA c, LPVOID x), (c, x))
FWD(DirectSoundCaptureEnumerateW, (LPDSENUMCALLBACKW c, LPVOID x), (c, x))
FWD(GetDeviceID, (LPCGUID a, LPGUID b), (a, b))
FWD(DirectSoundCaptureCreate8, (LPCGUID g, LPDIRECTSOUNDCAPTURE8 *c, LPUNKNOWN u), (g, c, u))
FWD(DllGetClassObject, (REFCLSID a, REFIID b, LPVOID *c), (a, b, c))
HRESULT WINAPI P_DllCanUnloadNow(void) { return S_FALSE; }
typedef HRESULT (WINAPI *T_FD)(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC, HWND, DWORD,
                               LPDIRECTSOUNDFULLDUPLEX *, LPDIRECTSOUNDCAPTUREBUFFER8 *, LPDIRECTSOUNDBUFFER8 *, LPUNKNOWN);
HRESULT WINAPI P_DirectSoundFullDuplexCreate(LPCGUID a, LPCGUID b, LPCDSCBUFFERDESC c, LPCDSBUFFERDESC d, HWND e, DWORD f,
                                             LPDIRECTSOUNDFULLDUPLEX *g, LPDIRECTSOUNDCAPTUREBUFFER8 *h, LPDIRECTSOUNDBUFFER8 *i, LPUNKNOWN j) {
    load_real();
    return ((T_FD)GetProcAddress(real, "DirectSoundFullDuplexCreate"))(a, b, c, d, e, f, g, h, i, j);
}

void sr_counts(LONG *r, LONG *v) { *r = created_real; *v = created_virtual; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID x) {
    (void)x;
    if (reason == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH];
        self = h;
        DisableThreadLibraryCalls(h);
        t0 = GetTickCount();
        InitializeCriticalSection(&logcs);
        InitializeCriticalSection(&hookcs);
        InitializeCriticalSection(&g_mix);
        sr_exe_dir_file("SunsetRacer2-dev.txt", p, MAX_PATH);
        sr_dev = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    }
    return TRUE;
}
