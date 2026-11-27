// The mixer thread: plays every virtual buffer in step with one real
// DirectSound output buffer, then hands the bus to the effects.
#include "sr2audio.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <xmmintrin.h>

CRITICAL_SECTION g_mix;
VBuf *g_voices;

#define OUT_BLOCKS 25
#define OUT_FRAMES (SR_BLOCK * OUT_BLOCKS)     // 250 ms ring
#define TAPS 16                                 // interpolator taps (-7..+8)
#define PHASES 256
#define HIST 16
#define MAXSRC (SR_BLOCK * 5 + HIST + TAPS + 8) // source frames per block (rates up to 240 kHz)

static IDirectSound *dev;            // the device Director creates its buffers on
static void *devCSB;                 // its real CreateSoundBuffer
static IDirectSound *pendingDev;
static void *pendingCSB;
static HANDLE thread, wakeEv;
static volatile LONG started, quit;
static VBuf *slotOwner[SR_NBIN];

static __declspec(align(16)) float kern[PHASES + 1][TAPS];
static __declspec(align(16)) float kdif[PHASES][TAPS];   // kern[p + 1] - kern[p]

// ---- kernel -------------------------------------------------------------------
static double bessel_i0(double x) {
    double s = 1, t = 1; int k;
    for (k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
static void build_kernel(void) {
    int p, k; const double beta = 8.0, pi = 3.14159265358979323846;
    for (p = 0; p <= PHASES; p++) {
        double f = (double)p / PHASES, sum = 0;
        for (k = 0; k < TAPS; k++) {
            double x = (k - 7) - f, h, w, r = x / 8.0;
            h = fabs(x) < 1e-12 ? 1.0 : sin(pi * x) / (pi * x);
            w = fabs(r) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
            kern[p][k] = (float)(h * w);
            sum += h * w;
        }
        for (k = 0; k < TAPS; k++) kern[p][k] = (float)(kern[p][k] / sum);   // unity DC gain
    }
    // phase 0 (and PHASES, which is phase 0 one sample later) are exact deltas
    for (k = 0; k < TAPS; k++) { kern[0][k] = k == 7 ? 1.0f : 0.0f; kern[PHASES][k] = k == 8 ? 1.0f : 0.0f; }
    for (p = 0; p < PHASES; p++) for (k = 0; k < TAPS; k++) kdif[p][k] = kern[p + 1][k] - kern[p][k];
}

static float hsum(__m128 v) {
    __m128 t = _mm_add_ps(v, _mm_movehl_ps(v, v));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
    return _mm_cvtss_f32(t);
}

// ---- voice mixing ----------------------------------------------------------------
static float lin[2][MAXSRC];

static float db_gain(LONG mb) { return mb <= DSBVOLUME_MIN ? 0.0f : (float)pow(10.0, mb / 2000.0); }

static void fire_range(VBuf *v, double a, double b) {
    // fires notifications whose offset lies in [a, b) frames (b may exceed frames once)
    int i;
    for (i = 0; i < v->nnotes; i++) {
        DWORD off = v->notes[i].dwOffset; double f;
        if (off == DSBPN_OFFSETSTOP || !v->notes[i].hEventNotify) continue;
        f = (double)(off / v->align);
        if ((f >= a && f < b) || (f + v->frames >= a && f + v->frames < b)) SetEvent(v->notes[i].hEventNotify);
    }
}
static void fire_stop(VBuf *v) {
    int i;
    for (i = 0; i < v->nnotes; i++)
        if (v->notes[i].dwOffset == DSBPN_OFFSETSTOP && v->notes[i].hEventNotify) SetEvent(v->notes[i].hEventNotify);
}

static void free_slot(VBuf *v) {
    if (v->bslot >= 0) { slotOwner[v->bslot] = NULL; v->bslot = -1; }
}
void mixer_voice_removed(VBuf *v) { free_slot(v); }

// Pan as a position tag (only with hrtf=1, mono sources): DirectSound pan
// -10000..10000 maps to azimuth -180..180 degrees, 0 = ahead.
static float pan_azimuth(LONG pan) { return pan * 180.0f / 10000.0f; }

static void mix_voice(VBuf *v, SrBus *b, const SrSettings *s) {
    const BYTE *mem = v->data->mem;
    double rate = v->freq ? v->freq : v->wfx.nSamplesPerSec;
    double step = rate / SR_RATE, pos = v->pos, endpos;
    long base = (long)floor(pos);                      // first frame after the history
    double fpos = pos - base;
    int nsrc = (int)floor(fpos + step * (SR_BLOCK - 1)) + 9;   // frames needed from base
    int ch = v->ch, i, k, c;
    float tl, tr, gl0, gr0, g;
    int binaural = 0;
    float *dl, *dr;

    if (nsrc + HIST > MAXSRC) nsrc = MAXSRC - HIST;
    // history, then fresh frames from the buffer
    if (v->fresh) { memset(v->hist, 0, sizeof v->hist); }
    for (c = 0; c < ch; c++) for (i = 0; i < HIST; i++) lin[c][i] = v->hist[c * HIST + i];
    for (i = 0; i < nsrc; i++) {
        long f = base + i;
        if (f >= (long)v->frames) {
            if (v->looping) f %= (long)v->frames;
            else { for (c = 0; c < ch; c++) lin[c][HIST + i] = 0; continue; }
        }
        if (v->bits == 16) {
            const short *p = (const short *)(mem + f * v->align);
            for (c = 0; c < ch; c++) lin[c][HIST + i] = p[c] * (1.0f / 32768.0f);
        } else {
            const BYTE *p = mem + f * v->align;
            for (c = 0; c < ch; c++) lin[c][HIST + i] = (p[c] - 128) * (1.0f / 128.0f);
        }
    }

    // gains (DirectSound rules: volume in 1/100 dB, pan attenuates the other side)
    g = db_gain(v->vol);
    tl = tr = g;
    if (s->hrtf && ch == 1 && v->pan != 0) binaural = 1;
    else if (v->pan < 0) tr = g * db_gain(v->pan);
    else if (v->pan > 0) tl = g * db_gain(-v->pan);
    if (binaural && v->bslot < 0) {
        for (k = 0; k < SR_NBIN; k++) if (!slotOwner[k]) { slotOwner[k] = v; v->bslot = k; break; }
        if (v->bslot < 0) {            // all slots taken: plain pan for this one
            binaural = 0;
            if (v->pan < 0) tr = g * db_gain(v->pan); else tl = g * db_gain(-v->pan);
        }
    }
    if (!binaural) free_slot(v);
    if (v->fresh) { v->gl = tl; v->gr = tr; v->fresh = 0; }
    gl0 = v->gl; gr0 = v->gr;

    if (binaural) {
        b->binUsed[v->bslot] = 1;
        b->binAz[v->bslot] = pan_azimuth(v->pan);
        dl = b->bin[v->bslot]; dr = NULL;
        tr = tl; gr0 = gl0;
    } else if (ch == 2) { dl = b->dryL; dr = b->dryR; }
    else { dl = b->dieL; dr = b->dieR; }

    {
        const float inv = 1.0f / SR_BLOCK;
        int direct = (step == 1.0 && fpos == 0.0);
        for (i = 0; i < SR_BLOCK; i++) {
            float a = (float)(i + 1) * inv;
            float gl = gl0 + (tl - gl0) * a, gr = gr0 + (tr - gr0) * a;
            float sl, sr;
            if (direct) {
                sl = lin[0][HIST + i];
                sr = ch == 2 ? lin[1][HIST + i] : sl;
            } else {
                double p = fpos + step * i;
                int ip = (int)p;
                float pf = (float)((p - ip) * PHASES);
                int ph = (int)pf; float fr = pf - ph;
                const float *k0 = kern[ph], *kd = kdif[ph];
                const float *x0 = &lin[0][HIST + ip - 7];
                __m128 f4 = _mm_set1_ps(fr);
                __m128 c0 = _mm_add_ps(_mm_load_ps(k0), _mm_mul_ps(_mm_load_ps(kd), f4));
                __m128 c1 = _mm_add_ps(_mm_load_ps(k0 + 4), _mm_mul_ps(_mm_load_ps(kd + 4), f4));
                __m128 c2 = _mm_add_ps(_mm_load_ps(k0 + 8), _mm_mul_ps(_mm_load_ps(kd + 8), f4));
                __m128 c3 = _mm_add_ps(_mm_load_ps(k0 + 12), _mm_mul_ps(_mm_load_ps(kd + 12), f4));
                __m128 a = _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, _mm_loadu_ps(x0)), _mm_mul_ps(c1, _mm_loadu_ps(x0 + 4))),
                                      _mm_add_ps(_mm_mul_ps(c2, _mm_loadu_ps(x0 + 8)), _mm_mul_ps(c3, _mm_loadu_ps(x0 + 12))));
                sl = hsum(a);
                if (ch == 2) {
                    const float *x1 = &lin[1][HIST + ip - 7];
                    a = _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, _mm_loadu_ps(x1)), _mm_mul_ps(c1, _mm_loadu_ps(x1 + 4))),
                                   _mm_add_ps(_mm_mul_ps(c2, _mm_loadu_ps(x1 + 8)), _mm_mul_ps(c3, _mm_loadu_ps(x1 + 12))));
                    sr = hsum(a);
                } else sr = sl;
            }
            if (binaural) { dl[i] += gl * sl; b->send[i] += gl * sl; }
            else {
                dl[i] += gl * sl;
                dr[i] += gr * sr;
                if (ch == 1) b->send[i] += 0.5f * (gl + gr) * sl;
            }
        }
    }
    v->gl = tl; v->gr = tr;

    // advance, keep the last HIST source frames as history
    endpos = pos + step * SR_BLOCK;
    {
        long nb = (long)floor(endpos);
        int off = (int)(nb - base);            // index of frame nb inside lin, minus HIST
        for (c = 0; c < ch; c++)
            for (i = 0; i < HIST; i++) {
                int j = HIST + off - HIST + i;
                v->hist[c * HIST + i] = (j >= 0 && j < HIST + nsrc) ? lin[c][j] : 0.0f;
            }
    }
    if (endpos >= v->frames) {
        if (v->looping) {
            fire_range(v, pos, endpos);
            endpos = fmod(endpos, (double)v->frames);
        } else {
            fire_range(v, pos, (double)v->frames);
            v->playing = 0;
            endpos = 0;                        // DirectSound rewinds a buffer that played out
            free_slot(v);
            fire_stop(v);
        }
    } else fire_range(v, pos, endpos);
    v->pos = endpos;
}

// ---- output -------------------------------------------------------------------------
static IDirectSoundBuffer *out;
static int outFloat;
static DWORD lastPlay;
static LONGLONG played, written;
static DWORD gapFrames;

static const GUID SubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
typedef HRESULT (STDMETHODCALLTYPE *PFN_CSB)(IDirectSound *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);

static void release_out(void) {
    if (out) { IDirectSoundBuffer_Stop(out); IDirectSoundBuffer_Release(out); out = NULL; }
}

static int create_out(void) {
    WAVEFORMATEXTENSIBLE w; DSBUFFERDESC d; HRESULT hr; int pass;
    for (pass = 0; pass < 2 && !out; pass++) {
        memset(&w, 0, sizeof w);
        w.Format.nChannels = 2;
        w.Format.nSamplesPerSec = SR_RATE;
        if (pass == 0) {
            w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            w.Format.wBitsPerSample = 32;
            w.Format.cbSize = 22;
            w.Samples.wValidBitsPerSample = 32;
            w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            w.SubFormat = SubtypeFloat;
        } else {
            w.Format.wFormatTag = WAVE_FORMAT_PCM;
            w.Format.wBitsPerSample = 16;
        }
        w.Format.nBlockAlign = w.Format.nChannels * w.Format.wBitsPerSample / 8;
        w.Format.nAvgBytesPerSec = w.Format.nBlockAlign * SR_RATE;
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
        d.dwBufferBytes = OUT_FRAMES * w.Format.nBlockAlign;
        d.lpwfxFormat = &w.Format;
        hr = ((PFN_CSB)devCSB)(dev, &d, &out, NULL);
        if (FAILED(hr)) { out = NULL; sr_log("output buffer (%s) failed: %08lx", pass ? "16-bit" : "float", hr); }
        else outFloat = pass == 0;
    }
    if (!out) return 0;
    sr_log("output buffer: %d Hz stereo %s, %d ms ring, %d ms blocks", SR_RATE, outFloat ? "float" : "16-bit",
           OUT_FRAMES * 1000 / SR_RATE, SR_BLOCK * 1000 / SR_RATE);
    return 1;
}

static void clear_out(void) {
    void *p1, *p2; DWORD n1, n2;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(out, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER))) {
        memset(p1, 0, n1);
        IDirectSoundBuffer_Unlock(out, p1, n1, p2, n2);
    }
    IDirectSoundBuffer_SetCurrentPosition(out, 0);
}

static void start_out(void) {
    DWORD p, w;
    clear_out();
    IDirectSoundBuffer_Play(out, 0, 0, DSBPLAY_LOOPING);
    IDirectSoundBuffer_GetCurrentPosition(out, &p, &w);
    lastPlay = p;
    played = 0;
    gapFrames = ((w + (outFloat ? 8 : 4) * OUT_FRAMES - p) % (OUT_FRAMES * (outFloat ? 8 : 4))) / (outFloat ? 8 : 4);
    written = gapFrames;               // first block goes right after the write cursor
}

static void write_out(const float *L, const float *R, int mute) {
    int fb = outFloat ? 8 : 4, i;
    DWORD off = (DWORD)(written % OUT_FRAMES) * fb;
    void *p1, *p2; DWORD n1, n2;
    if (FAILED(IDirectSoundBuffer_Lock(out, off, SR_BLOCK * fb, &p1, &n1, &p2, &n2, 0))) return;
    {
        void *ps[2] = { p1, p2 }; DWORD ns[2] = { n1, n2 }; int j = 0, seg;
        for (seg = 0; seg < 2; seg++) {
            int frames = (int)(ns[seg] / fb);
            if (!ps[seg]) continue;
            if (outFloat) {
                float *o = (float *)ps[seg];
                for (i = 0; i < frames; i++, j++) { o[2 * i] = mute ? 0 : L[j]; o[2 * i + 1] = mute ? 0 : R[j]; }
            } else {
                short *o = (short *)ps[seg];
                for (i = 0; i < frames; i++, j++) {
                    float l = mute ? 0 : L[j] * 32768.0f, r = mute ? 0 : R[j] * 32768.0f;
                    o[2 * i] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : (int)lrintf(l));
                    o[2 * i + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : (int)lrintf(r));
                }
            }
        }
    }
    IDirectSoundBuffer_Unlock(out, p1, n1, p2, n2);
}

// ---- capture (dev only) ----------------------------------------------------------------
typedef struct { HANDLE h; DWORD frames; } Wav;
static Wav capMix, capDry;
static int capIndex;

static void wav_header(Wav *w) {
    struct { char riff[4]; DWORD size; char wave[4]; char fmt[4]; DWORD fsz; WORD tag, ch; DWORD rate, bps; WORD align, bits;
             char data[4]; DWORD dsz; } h;
    DWORD n, pos;
    memcpy(h.riff, "RIFF", 4); memcpy(h.wave, "WAVE", 4); memcpy(h.fmt, "fmt ", 4); memcpy(h.data, "data", 4);
    h.fsz = 16; h.tag = 3; h.ch = 2; h.rate = SR_RATE; h.bps = SR_RATE * 8; h.align = 8; h.bits = 32;
    h.dsz = w->frames * 8; h.size = 36 + h.dsz;
    pos = SetFilePointer(w->h, 0, NULL, FILE_CURRENT);
    SetFilePointer(w->h, 0, NULL, FILE_BEGIN);
    WriteFile(w->h, &h, sizeof h, &n, NULL);
    SetFilePointer(w->h, pos ? pos : sizeof h, NULL, FILE_BEGIN);
}
static void wav_open(Wav *w, const char *kind) {
    char name[64], p[MAX_PATH];
    wsprintfA(name, "SunsetRacer2-audio-%s-%03d.wav", kind, capIndex);
    sr_exe_dir_file(name, p, MAX_PATH);
    w->h = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    w->frames = 0;
    if (w->h != INVALID_HANDLE_VALUE) wav_header(w);
    sr_log("capture -> %s", p);
}
static void wav_write(Wav *w, const float *L, const float *R) {
    float buf[SR_BLOCK * 2]; DWORD n; int i;
    if (w->h == INVALID_HANDLE_VALUE || !w->h) return;
    for (i = 0; i < SR_BLOCK; i++) { buf[2 * i] = L[i]; buf[2 * i + 1] = R[i]; }
    WriteFile(w->h, buf, sizeof buf, &n, NULL);
    w->frames += SR_BLOCK;
    if (w->frames % SR_RATE < SR_BLOCK) wav_header(w);
}
static void wav_close(Wav *w) {
    if (w->h && w->h != INVALID_HANDLE_VALUE) { wav_header(w); CloseHandle(w->h); }
    w->h = NULL;
}

// ---- the thread ---------------------------------------------------------------------------
static float busMem[6 + SR_NBIN][SR_BLOCK];
static float outL[SR_BLOCK], outR[SR_BLOCK], capL[SR_BLOCK], capR[SR_BLOCK];

typedef struct { LONGLONG n, sum, max, mixSum; int underruns, voicesMax; } Stats;
static Stats st;
static LONGLONG qpf;
void sr_counts(LONG *r, LONG *v);

static int render_block(const SrSettings *s, int capture) {
    SrBus b; VBuf *v; int i, nplay = 0;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    memset(busMem, 0, sizeof busMem);
    memset(&b, 0, sizeof b);
    b.dryL = busMem[0]; b.dryR = busMem[1]; b.dieL = busMem[2]; b.dieR = busMem[3]; b.send = busMem[4];
    for (i = 0; i < SR_NBIN; i++) b.bin[i] = busMem[6 + i];
    EnterCriticalSection(&g_mix);
    for (v = g_voices; v; v = v->next) if (v->playing && v->frames) { mix_voice(v, &b, s); nplay++; }
    LeaveCriticalSection(&g_mix);
    QueryPerformanceCounter(&t1);
    if (capture) {
        for (i = 0; i < SR_BLOCK; i++) {
            float m = 0; int k;
            for (k = 0; k < SR_NBIN; k++) m += b.bin[k][i];
            capL[i] = b.dryL[i] + b.dieL[i] + m;
            capR[i] = b.dryR[i] + b.dieR[i] + m;
        }
    }
    fx_process(&b, s, outL, outR);
    QueryPerformanceCounter(&t2);
    st.n++;
    st.sum += t2.QuadPart - t0.QuadPart;
    st.mixSum += t1.QuadPart - t0.QuadPart;
    if (t2.QuadPart - t0.QuadPart > st.max) st.max = t2.QuadPart - t0.QuadPart;
    if (nplay > st.voicesMax) st.voicesMax = nplay;
    return nplay;
}

static void report(void) {
    char fx[256]; LONG r, v;
    if (!sr_dev || !st.n) return;
    fx_stats(fx, sizeof fx);
    sr_counts(&r, &v);
    sr_log("mixer: %lld blocks, render avg %.3f ms (mix %.3f) max %.3f ms per 10 ms block, voices max %d, underruns %d, "
           "lead %d ms, buffers made real %ld virtual %ld | %s",
           st.n, st.sum * 1000.0 / qpf / st.n, st.mixSum * 1000.0 / qpf / st.n, st.max * 1000.0 / qpf,
           st.voicesMax, st.underruns, (int)((written - played) * 1000 / SR_RATE), r, v, fx);
    memset(&st, 0, sizeof st);
}

static DWORD WINAPI mixer_main(LPVOID x) {
    typedef HANDLE (WINAPI *PFN_AvSet)(LPCWSTR, LPDWORD);
    HMODULE avrt = LoadLibraryA("avrt.dll");
    PFN_AvSet avset = avrt ? (PFN_AvSet)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
    DWORD task = 0, lastReport = GetTickCount(), idleSince = 0, lastPoll = GetTickCount(), startTick = 0;
    int running = 0, capturing = 0;
    LARGE_INTEGER f;
    (void)x;
    QueryPerformanceFrequency(&f); qpf = f.QuadPart;
    if (!avset || !avset(L"Pro Audio", &task)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    _mm_setcsr(_mm_getcsr() | 0x8040);           // flush denormals to zero (reverb tails)
    build_kernel();
    while (!quit) {
        SrSettings s; SrDevOpts d; int anyPlaying = 0; VBuf *v;
        sr_settings_get(&s);
        sr_devopts_get(&d);

        // device change (Director made a new device)
        EnterCriticalSection(&g_mix);
        if (pendingDev) {
            IDirectSound *nd = pendingDev; void *nc = pendingCSB;
            pendingDev = NULL;
            LeaveCriticalSection(&g_mix);
            release_out();
            running = 0;
            if (dev) IDirectSound_Release(dev);
            dev = nd; devCSB = nc;
            EnterCriticalSection(&g_mix);
        }
        for (v = g_voices; v; v = v->next) if (v->playing) { anyPlaying = 1; break; }
        LeaveCriticalSection(&g_mix);

        if (!running) {
            if (!anyPlaying) { WaitForSingleObject(wakeEv, 500); continue; }
            if (!out && !create_out()) { WaitForSingleObject(wakeEv, 1000); continue; }
            start_out();
            lastPoll = startTick = GetTickCount();
            running = 1;
            idleSince = 0;
            sr_log("output started, write gap %lu ms", gapFrames * 1000 / SR_RATE);
        }

        if (d.capture != capturing) {
            if (d.capture) { capIndex++; wav_open(&capMix, "mix"); wav_open(&capDry, "dry"); }
            else { wav_close(&capMix); wav_close(&capDry); }
            capturing = d.capture;
        }

        {
            DWORD p, w; int fb = outFloat ? 8 : 4; LONGLONG target; DWORD now = GetTickCount();
            IDirectSoundBuffer_GetCurrentPosition(out, &p, &w);
            if (now - lastPoll > OUT_FRAMES * 1000 / SR_RATE * 3 / 4) {
                // stalled for most of the ring: position deltas are no longer trustworthy
                st.underruns++;
                played = written;
            } else played += ((p + OUT_FRAMES * fb - lastPlay) % (OUT_FRAMES * fb)) / fb;
            lastPoll = now;
            lastPlay = p;
            {
                DWORD g = ((w + OUT_FRAMES * fb - p) % (OUT_FRAMES * fb)) / fb;
                if (g > gapFrames) gapFrames = g;
            }
            if (written < played + gapFrames) {          // fell behind the write cursor
                if (now - startTick > 300) st.underruns++;   // (the gap only shows once the buffer runs)
                written = played + gapFrames;
            }
            target = played + gapFrames + SR_BLOCK + SR_BLOCK / 2;
            while (written < target) {
                int n = render_block(&s, capturing);
                if (capturing) { wav_write(&capMix, outL, outR); wav_write(&capDry, capL, capR); }
                write_out(outL, outR, d.mute);
                written += SR_BLOCK;
                if (n) anyPlaying = 1;
            }
        }

        // The output keeps running while the mode is on (a restarted
        // DirectSound buffer can take a moment to sound); it stops once the
        // mode is off and every virtual buffer is gone.
        if (!anyPlaying && !fx_busy() && !s.steamaudio && !g_voices) {
            if (!idleSince) idleSince = GetTickCount();
            else if (GetTickCount() - idleSince > 2000) {
                IDirectSoundBuffer_Stop(out);
                running = 0;
                sr_log("output idle, stopped");
                continue;
            }
        } else idleSince = 0;

        if (GetTickCount() - lastReport >= 5000) { report(); lastReport = GetTickCount(); }
        Sleep(2);
    }
    if (capturing) { wav_close(&capMix); wav_close(&capDry); }
    report();
    timeEndPeriod(1);
    return 0;
}

IDirectSound *mixer_device(void) { return started ? dev : NULL; }

// Director released its last reference to the device (it quits, or drops the
// device): stop the thread, release the output buffer, then our reference.
void mixer_shutdown(void (*release_dev)(IDirectSound *)) {
    IDirectSound *d;
    if (!started) return;
    quit = 1;
    SetEvent(wakeEv);
    if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0) sr_log("mixer thread did not stop");
    CloseHandle(thread);
    release_out();
    EnterCriticalSection(&g_mix);
    d = dev; dev = NULL;
    if (pendingDev) { release_dev(pendingDev); pendingDev = NULL; }
    started = 0; quit = 0;
    LeaveCriticalSection(&g_mix);
    if (d) release_dev(d);
    sr_log("mixer stopped with the device");
}

void mixer_attach_device(IDirectSound *ds, void *realCSB) {
    EnterCriticalSection(&g_mix);
    if (!started) {
        dev = ds; devCSB = realCSB; IDirectSound_AddRef(ds);
        wakeEv = CreateEventA(NULL, FALSE, FALSE, NULL);
        started = 1;
        thread = CreateThread(NULL, 0, mixer_main, NULL, 0, NULL);
    } else if (ds != dev && ds != pendingDev) {
        if (pendingDev) IDirectSound_Release(pendingDev);
        pendingDev = ds; pendingCSB = realCSB; IDirectSound_AddRef(ds);
        SetEvent(wakeEv);
    }
    LeaveCriticalSection(&g_mix);
}

void mixer_wake(void) { if (wakeEv) SetEvent(wakeEv); }
