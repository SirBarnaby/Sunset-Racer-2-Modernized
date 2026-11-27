// A software IDirectSoundBuffer8 (+ IDirectSoundNotify) that behaves like a
// DirectSound secondary buffer. The memory lives here; mixer.c moves the play
// cursor in step with the real output device and fires the notifications.
#include "sr2audio.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const IDirectSoundBuffer8Vtbl vtbl;
static const IDirectSoundNotifyVtbl ntbl;
static volatile LONG next_id;

#define VB(p) ((VBuf *)(p))
#define VN(p) ((VBuf *)((BYTE *)(p) - offsetof(VBuf, lpNotifyVtbl)))

int vbuf_is(void *p) { return p && *(const void **)p == (const void *)&vtbl; }

static int tracing(void) { SrDevOpts d; if (!sr_dev) return 0; sr_devopts_get(&d); return d.trace; }

DWORD vbuf_read_margin(VBuf *v) {
    double rate = v->freq ? v->freq : v->wfx.nSamplesPerSec;
    return (DWORD)ceil(SR_BLOCK * rate / SR_RATE) + 16;
}

// ---- IUnknown ---------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE B_QueryInterface(LPDIRECTSOUNDBUFFER8 This, REFIID riid, LPVOID *out) {
    VBuf *v = VB(This);
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundBuffer) || IsEqualIID(riid, &IID_IDirectSoundBuffer8)) {
        *out = v; InterlockedIncrement(&v->ref); return S_OK;
    }
    if (IsEqualIID(riid, &IID_IDirectSoundNotify) && (v->flags & DSBCAPS_CTRLPOSITIONNOTIFY)) {
        *out = &v->lpNotifyVtbl; InterlockedIncrement(&v->ref); return S_OK;
    }
    if (sr_dev) sr_log("buf%d QueryInterface for an unsupported interface {%08lx-...}", v->id, riid->Data1);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE B_AddRef(LPDIRECTSOUNDBUFFER8 This) { return InterlockedIncrement(&VB(This)->ref); }

static void data_release(VData *d) {
    if (InterlockedDecrement(&d->ref) == 0) { free(d->mem); free(d); }
}

static ULONG STDMETHODCALLTYPE B_Release(LPDIRECTSOUNDBUFFER8 This) {
    VBuf *v = VB(This);
    LONG r = InterlockedDecrement(&v->ref);
    if (r == 0) {
        EnterCriticalSection(&g_mix);
        if (v->prev) v->prev->next = v->next; else g_voices = v->next;
        if (v->next) v->next->prev = v->prev;
        mixer_voice_removed(v);
        LeaveCriticalSection(&g_mix);
        if (tracing()) sr_log("buf%d released", v->id);
        free(v->notes);
        data_release(v->data);
        v->lpVtbl = NULL;
        free(v);
    }
    return (ULONG)r;
}

// ---- getters ------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE B_GetCaps(LPDIRECTSOUNDBUFFER8 This, LPDSBCAPS c) {
    VBuf *v = VB(This);
    if (!c || c->dwSize < sizeof(DSBCAPS)) return DSERR_INVALIDPARAM;
    c->dwFlags = (v->flags & ~DSBCAPS_LOCDEFER) | DSBCAPS_LOCSOFTWARE;
    c->dwBufferBytes = v->bytes;
    c->dwUnlockTransferRate = 0;
    c->dwPlayCpuOverhead = 0;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE B_GetCurrentPosition(LPDIRECTSOUNDBUFFER8 This, LPDWORD play, LPDWORD write) {
    VBuf *v = VB(This); DWORD p, w;
    EnterCriticalSection(&g_mix);
    p = (DWORD)v->pos;
    if (p >= v->frames) p = 0;
    w = p;
    if (v->playing) w = (p + vbuf_read_margin(v)) % v->frames;
    LeaveCriticalSection(&g_mix);
    if (play) *play = p * v->align;
    if (write) *write = w * v->align;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE B_GetFormat(LPDIRECTSOUNDBUFFER8 This, LPWAVEFORMATEX f, DWORD size, LPDWORD written) {
    VBuf *v = VB(This); DWORD need = sizeof(WAVEFORMATEX);
    if (!f) { if (!written) return DSERR_INVALIDPARAM; *written = need; return DS_OK; }
    if (size < sizeof(PCMWAVEFORMAT)) return DSERR_INVALIDPARAM;
    memcpy(f, &v->wfx, size < need ? size : need);
    if (written) *written = size < need ? size : need;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE B_GetVolume(LPDIRECTSOUNDBUFFER8 This, LPLONG out) {
    VBuf *v = VB(This);
    if (!out) return DSERR_INVALIDPARAM;
    if (!(v->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    *out = v->vol; return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_GetPan(LPDIRECTSOUNDBUFFER8 This, LPLONG out) {
    VBuf *v = VB(This);
    if (!out) return DSERR_INVALIDPARAM;
    if (!(v->flags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    *out = v->pan; return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_GetFrequency(LPDIRECTSOUNDBUFFER8 This, LPDWORD out) {
    VBuf *v = VB(This);
    if (!out) return DSERR_INVALIDPARAM;
    if (!(v->flags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    *out = v->freq ? v->freq : v->wfx.nSamplesPerSec; return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_GetStatus(LPDIRECTSOUNDBUFFER8 This, LPDWORD out) {
    VBuf *v = VB(This); DWORD s = 0;
    if (!out) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&g_mix);
    if (v->playing) s = DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0);
    LeaveCriticalSection(&g_mix);
    *out = s; return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_Initialize(LPDIRECTSOUNDBUFFER8 This, LPDIRECTSOUND ds, LPCDSBUFFERDESC d) {
    (void)This; (void)ds; (void)d; return DSERR_ALREADYINITIALIZED;
}

// ---- data -----------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE B_Lock(LPDIRECTSOUNDBUFFER8 This, DWORD off, DWORD n, LPVOID *p1, LPDWORD n1,
                                        LPVOID *p2, LPDWORD n2, DWORD flags) {
    VBuf *v = VB(This);
    if (!p1 || !n1) return DSERR_INVALIDPARAM;
    if (flags & DSBLOCK_FROMWRITECURSOR) { DWORD w; B_GetCurrentPosition(This, NULL, &w); off = w; }
    if (flags & DSBLOCK_ENTIREBUFFER) n = v->bytes;
    if (off >= v->bytes || n == 0 || n > v->bytes) return DSERR_INVALIDPARAM;
    *p1 = v->data->mem + off;
    if (off + n <= v->bytes) {
        *n1 = n;
        if (p2) *p2 = NULL;
        if (n2) *n2 = 0;
    } else {
        *n1 = v->bytes - off;
        if (p2) { *p2 = v->data->mem; if (n2) *n2 = n - *n1; }
        else if (n2) *n2 = 0;
    }
    if (tracing()) sr_log("buf%d Lock off=%lu n=%lu flags=%lx (play %lu)", v->id, off, n, flags, (DWORD)v->pos * v->align);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_Unlock(LPDIRECTSOUNDBUFFER8 This, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) {
    (void)This; (void)p1; (void)n1; (void)p2; (void)n2; return DS_OK;
}

// ---- transport ---------------------------------------------------------------------
static void fire_stop(VBuf *v) {
    int i;
    for (i = 0; i < v->nnotes; i++)
        if (v->notes[i].dwOffset == DSBPN_OFFSETSTOP && v->notes[i].hEventNotify) SetEvent(v->notes[i].hEventNotify);
}

static HRESULT STDMETHODCALLTYPE B_Play(LPDIRECTSOUNDBUFFER8 This, DWORD r, DWORD prio, DWORD flags) {
    VBuf *v = VB(This); int was;
    (void)r; (void)prio;
    EnterCriticalSection(&g_mix);
    was = v->playing;
    v->looping = (flags & DSBPLAY_LOOPING) != 0;
    if (!was) { v->playing = 1; v->fresh = 1; }
    LeaveCriticalSection(&g_mix);
    if (tracing()) sr_log("buf%d Play flags=%lx at %lu%s", v->id, flags, (DWORD)v->pos * v->align, was ? " (already playing)" : "");
    if (!was) mixer_wake();
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_SetCurrentPosition(LPDIRECTSOUNDBUFFER8 This, DWORD p) {
    VBuf *v = VB(This);
    if (p >= v->bytes) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&g_mix);
    v->pos = (double)(p / v->align);
    v->fresh = 1;
    LeaveCriticalSection(&g_mix);
    if (tracing()) sr_log("buf%d SetCurrentPosition %lu", v->id, p);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_SetFormat(LPDIRECTSOUNDBUFFER8 This, LPCWAVEFORMATEX f) {
    (void)This; (void)f; return DSERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE B_SetVolume(LPDIRECTSOUNDBUFFER8 This, LONG vol) {
    VBuf *v = VB(This);
    if (!(v->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (vol < DSBVOLUME_MIN || vol > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    v->vol = vol;
    if (tracing()) sr_log("buf%d SetVolume %ld", v->id, vol);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_SetPan(LPDIRECTSOUNDBUFFER8 This, LONG pan) {
    VBuf *v = VB(This);
    if (!(v->flags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    v->pan = pan;
    if (tracing()) sr_log("buf%d SetPan %ld", v->id, pan);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_SetFrequency(LPDIRECTSOUNDBUFFER8 This, DWORD f) {
    VBuf *v = VB(This);
    if (!(v->flags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    if (f != DSBFREQUENCY_ORIGINAL && (f < DSBFREQUENCY_MIN || f > DSBFREQUENCY_MAX)) return DSERR_INVALIDPARAM;
    v->freq = f;
    if (tracing()) sr_log("buf%d SetFrequency %lu", v->id, f);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_Stop(LPDIRECTSOUNDBUFFER8 This) {
    VBuf *v = VB(This); int was;
    EnterCriticalSection(&g_mix);
    was = v->playing;
    v->playing = 0;
    if (was) fire_stop(v);
    LeaveCriticalSection(&g_mix);
    if (tracing()) sr_log("buf%d Stop at %lu%s", v->id, (DWORD)v->pos * v->align, was ? "" : " (not playing)");
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_Restore(LPDIRECTSOUNDBUFFER8 This) { (void)This; return DS_OK; }
static HRESULT STDMETHODCALLTYPE B_SetFX(LPDIRECTSOUNDBUFFER8 This, DWORD n, LPDSEFFECTDESC d, LPDWORD r) {
    (void)This; (void)n; (void)d; (void)r; return DSERR_CONTROLUNAVAIL;
}
static HRESULT STDMETHODCALLTYPE B_AcquireResources(LPDIRECTSOUNDBUFFER8 This, DWORD f, DWORD n, LPDWORD r) {
    (void)This; (void)f;
    if (r) { DWORD i; for (i = 0; i < n; i++) r[i] = DSFXR_UNALLOCATED; }
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE B_GetObjectInPath(LPDIRECTSOUNDBUFFER8 This, REFGUID g, DWORD i, REFGUID iid, LPVOID *o) {
    (void)This; (void)g; (void)i; (void)iid; if (o) *o = NULL; return DSERR_CONTROLUNAVAIL;
}

static const IDirectSoundBuffer8Vtbl vtbl = {
    B_QueryInterface, B_AddRef, B_Release, B_GetCaps, B_GetCurrentPosition, B_GetFormat, B_GetVolume, B_GetPan,
    B_GetFrequency, B_GetStatus, B_Initialize, B_Lock, B_Play, B_SetCurrentPosition, B_SetFormat, B_SetVolume,
    B_SetPan, B_SetFrequency, B_Stop, B_Unlock, B_Restore, B_SetFX, B_AcquireResources, B_GetObjectInPath
};

// ---- IDirectSoundNotify --------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE N_QueryInterface(LPDIRECTSOUNDNOTIFY This, REFIID riid, LPVOID *out) {
    return B_QueryInterface((LPDIRECTSOUNDBUFFER8)VN(This), riid, out);
}
static ULONG STDMETHODCALLTYPE N_AddRef(LPDIRECTSOUNDNOTIFY This) { return B_AddRef((LPDIRECTSOUNDBUFFER8)VN(This)); }
static ULONG STDMETHODCALLTYPE N_Release(LPDIRECTSOUNDNOTIFY This) { return B_Release((LPDIRECTSOUNDBUFFER8)VN(This)); }
static HRESULT STDMETHODCALLTYPE N_SetNotificationPositions(LPDIRECTSOUNDNOTIFY This, DWORD n, LPCDSBPOSITIONNOTIFY p) {
    VBuf *v = VN(This); DSBPOSITIONNOTIFY *copy = NULL; DWORD i;
    if (n && !p) return DSERR_INVALIDPARAM;
    for (i = 0; i < n; i++)
        if (p[i].dwOffset != DSBPN_OFFSETSTOP && p[i].dwOffset >= v->bytes) return DSERR_INVALIDPARAM;
    if (n) { copy = (DSBPOSITIONNOTIFY *)malloc(n * sizeof *copy); if (!copy) return DSERR_OUTOFMEMORY; memcpy(copy, p, n * sizeof *copy); }
    EnterCriticalSection(&g_mix);
    if (v->playing) { LeaveCriticalSection(&g_mix); free(copy); return DSERR_INVALIDCALL; }
    free(v->notes);
    v->notes = copy;
    v->nnotes = (int)n;
    LeaveCriticalSection(&g_mix);
    if (tracing()) {
        char b[512]; int k = 0;
        for (i = 0; i < n && k < 480; i++) k += wsprintfA(b + k, " %ld", (LONG)p[i].dwOffset);
        b[k] = 0;
        sr_log("buf%d SetNotificationPositions %lu:%s", v->id, n, b);
    }
    return DS_OK;
}
static const IDirectSoundNotifyVtbl ntbl = { N_QueryInterface, N_AddRef, N_Release, N_SetNotificationPositions };

// ---- creation ----------------------------------------------------------------------------
static VBuf *alloc_voice(VData *data, const WAVEFORMATEX *w, DWORD flags, DWORD bytes) {
    VBuf *v = (VBuf *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->lpVtbl = &vtbl;
    v->lpNotifyVtbl = &ntbl;
    v->ref = 1;
    v->id = InterlockedIncrement(&next_id);
    v->data = data;
    memcpy(&v->wfx, w, sizeof(PCMWAVEFORMAT));
    v->wfx.cbSize = 0;
    v->flags = flags;
    v->bytes = bytes;
    v->ch = w->nChannels;
    v->bits = w->wBitsPerSample;
    v->align = w->nBlockAlign;
    v->frames = bytes / v->align;
    v->bslot = -1;
    return v;
}

static void link_voice(VBuf *v) {
    EnterCriticalSection(&g_mix);
    v->next = g_voices; v->prev = NULL;
    if (g_voices) g_voices->prev = v;
    g_voices = v;
    LeaveCriticalSection(&g_mix);
}

HRESULT vbuf_create(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out) {
    const WAVEFORMATEX *w = d->lpwfxFormat;
    DWORD bytes = d->dwBufferBytes - d->dwBufferBytes % w->nBlockAlign;
    VData *data = (VData *)calloc(1, sizeof *data);
    VBuf *v;
    if (!data) return DSERR_OUTOFMEMORY;
    data->ref = 1;
    data->bytes = bytes;
    data->mem = (BYTE *)malloc(bytes);
    if (!data->mem) { free(data); return DSERR_OUTOFMEMORY; }
    memset(data->mem, w->wBitsPerSample == 8 ? 0x80 : 0, bytes);
    v = alloc_voice(data, w, d->dwFlags, bytes);
    if (!v) { data_release(data); return DSERR_OUTOFMEMORY; }
    link_voice(v);
    *out = (LPDIRECTSOUNDBUFFER)v;
    if (tracing()) sr_log("buf%d created flags=%lx bytes=%lu %lu Hz %d ch %d bit", v->id, d->dwFlags, bytes,
                          w->nSamplesPerSec, w->nChannels, w->wBitsPerSample);
    return DS_OK;
}

HRESULT vbuf_duplicate(VBuf *src, LPDIRECTSOUNDBUFFER *out) {
    VBuf *v;
    InterlockedIncrement(&src->data->ref);
    v = alloc_voice(src->data, &src->wfx, src->flags, src->bytes);
    if (!v) { data_release(src->data); return DSERR_OUTOFMEMORY; }
    v->vol = src->vol; v->pan = src->pan; v->freq = src->freq;
    link_voice(v);
    *out = (LPDIRECTSOUNDBUFFER)v;
    if (tracing()) sr_log("buf%d duplicated from buf%d", v->id, src->id);
    return DS_OK;
}
