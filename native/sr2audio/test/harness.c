// Test harness for the sr2audio proxy. Runs beside dsound.dll + phonon.dll in
// a folder that also holds SunsetRacer2-dev.txt (so the proxy logs) and is
// always silent on the speakers (mute=1 for the proxy, zero samples and
// minimum volume for real DirectSound).
//
//   sr2audio-test semantics real|proxy   DirectSound behaviour the game relies on
//   sr2audio-test render                 scripted scenario captured to WAV
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef HRESULT (WINAPI *PFN_Create)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
static IDirectSound *ds;
static LARGE_INTEGER qf, qt0;
static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (t.QuadPart - qt0.QuadPart) * 1000.0 / qf.QuadPart; }

static void put(const char *name, const char *text) {
    char p[MAX_PATH], *s; FILE *f;
    GetModuleFileNameA(NULL, p, MAX_PATH); s = strrchr(p, '\\'); strcpy(s + 1, name);
    f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}

static int open_device(int real) {
    char p[MAX_PATH]; HMODULE m; PFN_Create c;
    if (real) { GetSystemDirectoryA(p, MAX_PATH); strcat(p, "\\dsound.dll"); }
    else { char *s; GetModuleFileNameA(NULL, p, MAX_PATH); s = strrchr(p, '\\'); strcpy(s + 1, "dsound.dll"); }
    m = LoadLibraryA(p);
    if (!m) { printf("cannot load %s\n", p); return 0; }
    c = (PFN_Create)GetProcAddress(m, (LPCSTR)1);
    if (FAILED(c(NULL, &ds, NULL))) { printf("DirectSoundCreate failed\n"); return 0; }
    IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    return 1;
}

static IDirectSoundBuffer *make_buffer(DWORD rate, int ch, DWORD bytes) {
    WAVEFORMATEX w; DSBUFFERDESC d; IDirectSoundBuffer *b = NULL;
    memset(&w, 0, sizeof w);
    w.wFormatTag = WAVE_FORMAT_PCM; w.nChannels = (WORD)ch; w.nSamplesPerSec = rate; w.wBitsPerSample = 16;
    w.nBlockAlign = (WORD)(2 * ch); w.nAvgBytesPerSec = rate * w.nBlockAlign;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d; d.dwFlags = 0x81E0; d.dwBufferBytes = bytes; d.lpwfxFormat = &w;   // Director's flags
    if (FAILED(IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL))) return NULL;
    return b;
}

static void fill(IDirectSoundBuffer *b, DWORD off, DWORD n, const short *src) {
    void *p1, *p2; DWORD n1, n2;
    if (FAILED(IDirectSoundBuffer_Lock(b, off, n, &p1, &n1, &p2, &n2, 0))) return;
    if (src) { memcpy(p1, src, n1); if (p2) memcpy(p2, (const BYTE *)src + n1, n2); }
    else { memset(p1, 0, n1); if (p2) memset(p2, 0, n2); }
    IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
}

// ---------------------------------------------------------------------------------------
static int traceOn;
static int semantics(int real) {
    IDirectSoundBuffer *b; IDirectSoundNotify *n; DSBPOSITIONNOTIFY pn[3]; HANDLE ev[3]; DWORD bytes = 88200, st, p, w;
    double t; int i, hits[3] = { 0, 0, 0 }; HRESULT hr;
    if (!real) put("SunsetRacer2-fx.ini", "steamaudio=1\r\nreverb=0\r\n");
    put("SunsetRacer2-audio-dev.txt", traceOn ? "mute=1\r\ntrace=1\r\n" : "mute=1\r\n");
    if (!open_device(real)) return 1;
    printf("== %s DirectSound ==\n", real ? "real" : "proxy");
    if (!real) Sleep(3000);                            // Steam Audio loads first
    b = make_buffer(44100, 1, bytes);
    if (!b) { printf("CreateSoundBuffer failed\n"); return 1; }
    IDirectSoundBuffer_SetVolume(b, DSBVOLUME_MIN);
    fill(b, 0, bytes, NULL);
    hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&n);
    printf("QueryInterface(IDirectSoundNotify) hr=%08lx\n", hr);
    for (i = 0; i < 3; i++) ev[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
    pn[0].dwOffset = 0; pn[1].dwOffset = bytes / 2; pn[2].dwOffset = DSBPN_OFFSETSTOP;
    for (i = 0; i < 3; i++) pn[i].hEventNotify = ev[i];
    hr = IDirectSoundNotify_SetNotificationPositions(n, 3, pn);
    printf("SetNotificationPositions hr=%08lx\n", hr);

    // 1. one-shot play to the end
    t = now_ms();
    IDirectSoundBuffer_Play(b, 0, 0, 0);
    IDirectSoundBuffer_GetCurrentPosition(b, &p, &w);
    printf("just after Play: play=%lu write=%lu (write gap %.1f ms)\n", p, w, (w - p) / 88.2);
    hr = IDirectSoundNotify_SetNotificationPositions(n, 3, pn);
    printf("SetNotificationPositions while playing hr=%08lx\n", hr);
    for (;;) {
        DWORD r = WaitForMultipleObjects(3, ev, FALSE, 3000);
        if (r == WAIT_TIMEOUT) { printf("timeout\n"); break; }
        hits[r - WAIT_OBJECT_0]++;
        printf("  event %s at %.0f ms\n", r == 0 ? "offset 0" : r == 1 ? "half" : "STOP", now_ms() - t);
        if (r == 2) break;
    }
    IDirectSoundBuffer_GetStatus(b, &st);
    IDirectSoundBuffer_GetCurrentPosition(b, &p, &w);
    printf("after the end: status=%lx play=%lu write=%lu\n", st, p, w);

    // 2. Stop on a stopped buffer
    IDirectSoundBuffer_Stop(b);
    printf("Stop on a stopped buffer fires STOP: %s\n", WaitForSingleObject(ev[2], 200) == WAIT_OBJECT_0 ? "yes" : "no");

    // 3. looping: cursor rate and notification count over 5 s
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    for (i = 0; i < 3; i++) { ResetEvent(ev[i]); hits[i] = 0; }
    IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
    t = now_ms();
    {
        double total = 0, last = 0, gapMax = 0, gapSum = 0; int samples = 0; DWORD lastp;
        IDirectSoundBuffer_GetCurrentPosition(b, &lastp, &w);
        while (now_ms() - t < 5000) {
            DWORD r = WaitForMultipleObjects(3, ev, FALSE, 5);
            if (r < 3) hits[r]++;
            IDirectSoundBuffer_GetCurrentPosition(b, &p, &w);
            total += (double)((p + bytes - lastp) % bytes); lastp = p;
            { double g = (double)((w + bytes - p) % bytes) / 88.2; gapSum += g; if (g > gapMax) gapMax = g; samples++; }
            (void)last;
        }
        printf("looping 5 s: cursor rate %.1f frames/s (nominal 44100), notifications offset0=%d half=%d stop=%d\n",
               total / 2 / ((now_ms() - t) / 1000.0), hits[0], hits[1], hits[2]);
        printf("write gap avg %.1f ms max %.1f ms\n", gapSum / samples, gapMax);
    }
    IDirectSoundBuffer_Stop(b);
    printf("Stop while playing fires STOP: %s\n", WaitForSingleObject(ev[2], 200) == WAIT_OBJECT_0 ? "yes" : "no");
    IDirectSoundBuffer_GetCurrentPosition(b, &p, &w);
    printf("after Stop: play=%lu write=%lu (stays where it stopped: %s)\n", p, w, p == w ? "write==play" : "write!=play");

    // 4. SetCurrentPosition + GetFormat + GetCaps
    {
        DSBCAPS c; WAVEFORMATEX f; DWORD got; LONG v, pa; DWORD fr;
        IDirectSoundBuffer_SetCurrentPosition(b, 1001);
        IDirectSoundBuffer_GetCurrentPosition(b, &p, &w);
        printf("SetCurrentPosition(1001) -> play=%lu\n", p);
        c.dwSize = sizeof c; IDirectSoundBuffer_GetCaps(b, &c);
        printf("GetCaps flags=%lx bytes=%lu\n", c.dwFlags, c.dwBufferBytes);
        IDirectSoundBuffer_GetFormat(b, &f, sizeof f, &got);
        printf("GetFormat %lu Hz %u ch, %lu bytes\n", f.nSamplesPerSec, f.nChannels, got);
        IDirectSoundBuffer_GetVolume(b, &v); IDirectSoundBuffer_GetPan(b, &pa); IDirectSoundBuffer_GetFrequency(b, &fr);
        printf("volume %ld pan %ld frequency %lu\n", v, pa, fr);
        printf("SetFrequency(22050) hr=%08lx; SetVolume(1) hr=%08lx; SetPan(10001) hr=%08lx\n",
               IDirectSoundBuffer_SetFrequency(b, 22050), IDirectSoundBuffer_SetVolume(b, 1), IDirectSoundBuffer_SetPan(b, 10001));
    }
    IDirectSoundNotify_Release(n);
    printf("final Release -> %lu\n", IDirectSoundBuffer_Release(b));
    IDirectSound_Release(ds);
    return 0;
}

// ---------------------------------------------------------------------------------------
// render scenario
static short *make_tone(DWORD rate, int ch, double secs, double f0, double f1, double amp) {
    int n = (int)(rate * secs), i; short *s = (short *)malloc(n * ch * 2);
    for (i = 0; i < n; i++) {
        double t = (double)i / rate;
        s[i * ch] = (short)(amp * 32767 * sin(2 * 3.14159265358979 * f0 * t));
        if (ch == 2) s[i * ch + 1] = (short)(amp * 32767 * sin(2 * 3.14159265358979 * f1 * t));
    }
    return s;
}
static unsigned rng = 12345;
static short *make_burst(DWORD rate, double secs, double burst) {
    int n = (int)(rate * secs), nb = (int)(rate * burst), i; short *s = (short *)calloc(n, 2);
    for (i = 0; i < nb; i++) { rng = rng * 1664525 + 1013904223; s[i] = (short)((int)(rng >> 16) - 32768) / 2; }
    // 2 ms fade in and out so the burst has no click of its own
    for (i = 0; i < rate / 500; i++) { float g = (float)i / (rate / 500); s[i] = (short)(s[i] * g); s[nb - 1 - i] = (short)(s[nb - 1 - i] * g); }
    return s;
}

// plays a one-shot buffer holding the whole signal
static IDirectSoundBuffer *oneshot(DWORD rate, int ch, const short *sig, double secs, LONG vol, LONG pan) {
    DWORD bytes = (DWORD)(rate * secs) * ch * 2;
    IDirectSoundBuffer *b = make_buffer(rate, ch, bytes);
    if (!b) return NULL;
    fill(b, 0, bytes, sig);
    IDirectSoundBuffer_SetVolume(b, vol);
    IDirectSoundBuffer_SetPan(b, pan);
    IDirectSoundBuffer_Play(b, 0, 0, 0);
    return b;
}

static void ini(const char *s) { put("SunsetRacer2-fx.ini", s); }
static void mark(const char *what) { printf("%8.0f ms  %s\n", now_ms(), what); fflush(stdout); }

// Director-style streaming: 1 s ring, four notifications, refilled behind the cursor.
static void stream(DWORD rate, double secs, double f0, double f1, void (*during)(double)) {
    DWORD bytes = rate * 2, q = bytes / 4; int total = (int)(rate * secs), done = 0, i;
    IDirectSoundBuffer *b = make_buffer(rate, 1, bytes); IDirectSoundNotify *n; DSBPOSITIONNOTIFY pn[4]; HANDLE ev[4];
    short *chunk = (short *)malloc(q);
    double ph = 0, t0;
    if (!b) return;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&n);
    for (i = 0; i < 4; i++) { ev[i] = CreateEventA(NULL, FALSE, FALSE, NULL); pn[i].dwOffset = i * q; pn[i].hEventNotify = ev[i]; }
    IDirectSoundNotify_SetNotificationPositions(n, 4, pn);
#define GEN() for (i = 0; i < (int)(q / 2); i++, done++) { double t = (double)done / rate, f = f0 * pow(f1 / f0, t / secs); \
        ph += 2 * 3.14159265358979 * f / rate; chunk[i] = done < total ? (short)(0.4 * 32767 * sin(ph)) : 0; }
    for (i = 0; i < 4; i++) { int k; GEN(); fill(b, i * q, q, chunk); (void)k; }
    IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
    t0 = now_ms();
    // the cursor starts on offset 0, which fires at once (real DirectSound does the same)
    WaitForMultipleObjects(4, ev, FALSE, 2000);
    while (done < total + (int)rate) {
        DWORD r = WaitForMultipleObjects(4, ev, FALSE, 2000);
        if (r >= 4) { printf("stream: notification timeout\n"); break; }
        // cursor reached quarter r: the quarter before it has played, refill it
        GEN();
        fill(b, ((r + 3) % 4) * q, q, chunk);
        if (during) during((now_ms() - t0) / 1000.0);
    }
    IDirectSoundBuffer_Stop(b);
    IDirectSoundNotify_Release(n);
    IDirectSoundBuffer_Release(b);
    free(chunk);
}

static int roomSeq[] = { 3, 1, 2, 0, 3 };
static int lastSeg = -1;
static void switch_rooms(double t) {
    int seg = (int)(t / 1.5);
    if (seg != lastSeg && seg < 5) {
        char s[200];
        lastSeg = seg;
        sprintf(s, "steamaudio=1\r\nreverb=100\r\nhrtf=0\r\nenv_room=%d\r\nenv_wet=0\r\nenv_speed=120\r\nenv_paused=0\r\n", roomSeq[seg]);
        ini(s);
        { char m[64]; sprintf(m, "stream: room %d", roomSeq[seg]); mark(m); }
    }
}

static int render(void) {
    short *tone = make_tone(44100, 1, 3.0, 1000, 0, 0.5);
    short *st = make_tone(48000, 2, 2.0, 500, 700, 0.5);
    short *burst = make_burst(44100, 4.0, 0.03);
    IDirectSoundBuffer *b[16]; int nb = 0, r;
    char s[256];
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=0\r\n");
    put("SunsetRacer2-audio-dev.txt", "capture=1\r\nmute=1\r\n");
    if (!open_device(0)) return 1;
    Sleep(4000);                                       // Steam Audio builds its rooms

    mark("phase 1: 1 kHz sine, 44.1 kHz mono, volume -600, no race");
    b[nb++] = oneshot(44100, 1, tone, 3.0, -600, 0);
    Sleep(3300);
    mark("phase 2: 500/700 Hz stereo 48 kHz, volume 0");
    b[nb++] = oneshot(48000, 2, st, 2.0, 0, 0);
    Sleep(2300);
    for (r = 0; r < 4; r++) {
        sprintf(s, "steamaudio=1\r\nreverb=100\r\nhrtf=0\r\nenv_room=%d\r\nenv_wet=0\r\nenv_speed=100\r\nenv_paused=0\r\n", r);
        ini(s); Sleep(600);
        sprintf(s, "phase 3.%d: noise burst, room %d", r, r); mark(s);
        b[nb++] = oneshot(44100, 1, burst, 4.0, 0, 0);
        Sleep(4200);
    }
    mark("phase 4: streamed sweep, rooms 3,1,2,0,3 every 1.5 s");
    stream(37557, 7.5, 200, 4000, switch_rooms);
    Sleep(4000);
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=0\r\nenv_room=3\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=0\r\n"); Sleep(600);
    mark("phase 5: burst in the tunnel, pause after 300 ms");
    b[nb++] = oneshot(44100, 1, burst, 4.0, 0, 0);
    Sleep(300);
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=0\r\nenv_room=3\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=1\r\n");
    mark("phase 5: paused");
    Sleep(3000);
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=0\r\nenv_room=3\r\nenv_wet=100\r\nenv_speed=0\r\nenv_paused=0\r\n"); Sleep(600);
    mark("phase 6: burst in the tunnel, env_wet=100");
    b[nb++] = oneshot(44100, 1, burst, 4.0, 0, 0);
    Sleep(4200);
    ini("steamaudio=1\r\nreverb=0\r\nhrtf=1\r\nenv_room=0\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=0\r\n"); Sleep(600);
    mark("phase 7: hrtf=1, burst panned +5000 (right), then -5000 (left), then 10000 (behind)");
    b[nb++] = oneshot(44100, 1, burst, 1.5, 0, 5000); Sleep(1700);
    b[nb++] = oneshot(44100, 1, burst, 1.5, 0, -5000); Sleep(1700);
    b[nb++] = oneshot(44100, 1, burst, 1.5, 0, 10000); Sleep(1700);
    ini("steamaudio=1\r\nreverb=50\r\nhrtf=0\r\nenv_room=3\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=0\r\n"); Sleep(600);
    mark("phase 8: burst in the tunnel, reverb=50");
    b[nb++] = oneshot(44100, 1, burst, 4.0, 0, 0);
    Sleep(4200);
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=1\r\nenv_room=3\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=0\r\n"); Sleep(600);
    mark("phase 9: burst in the tunnel, hrtf=1 (binaural reverb)");
    b[nb++] = oneshot(44100, 1, burst, 4.0, 0, 0);
    Sleep(4200);
    mark("end");
    put("SunsetRacer2-audio-dev.txt", "capture=0\r\nmute=1\r\n");
    Sleep(800);
    mark("releasing buffers");
    while (nb > 0) { nb--; if (b[nb]) IDirectSoundBuffer_Release(b[nb]); else printf("buffer %d was never made\n", nb); }
    mark("releasing device");
    IDirectSound_Release(ds);
    mark("done");
    return 0;
}

// quick start/stop cycle with reverb running, to check a clean exit
static int quick(int how) {
    short *burst = make_burst(44100, 2.0, 0.03); IDirectSoundBuffer *b;
    ini("steamaudio=1\r\nreverb=100\r\nhrtf=1\r\nenv_room=3\r\nenv_wet=0\r\nenv_speed=0\r\nenv_paused=0\r\n");
    put("SunsetRacer2-audio-dev.txt", "mute=1\r\n");
    if (!open_device(0)) return 1;
    Sleep(3500);
    b = oneshot(44100, 1, burst, 2.0, 0, 3000);
    Sleep(1000);
    if (how == 1) { IDirectSoundBuffer_Release(b); IDirectSound_Release(ds); }
    printf("exiting (%d)\n", how); fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qt0);
    timeBeginPeriod(1);
    traceOn = argc >= 4 && !strcmp(argv[3], "trace");
    if (argc >= 3 && !strcmp(argv[1], "semantics")) return semantics(!strcmp(argv[2], "real"));
    if (argc >= 2 && !strcmp(argv[1], "render")) return render();
    if (argc >= 3 && !strcmp(argv[1], "quick")) return quick(atoi(argv[2]));
    printf("usage: sr2audio-test semantics real|proxy | render\n");
    return 2;
}
