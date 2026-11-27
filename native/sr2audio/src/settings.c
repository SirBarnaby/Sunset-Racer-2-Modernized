// Reads SunsetRacer2-fx.ini beside the exe. A background thread checks the
// file's last-write time every 250 ms (GetFileAttributesEx only) and re-reads
// it when it changed. The game thread only ever copies the cached values.
#include "sr2audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CRITICAL_SECTION cs;
static SrSettings cur;
static SrDevOpts dev;
static volatile LONG sa_on;
static wchar_t iniPath[MAX_PATH], devPath[MAX_PATH];
static int started;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Parses the whole file. Returns 0 on a parse error (then everything is off).
static int parse(const char *t, int n, SrSettings *s, SrDevOpts *d) {
    int i = 0;
    memset(s, 0, sizeof *s);
    if (d) memset(d, 0, sizeof *d);
    while (i < n) {
        char line[128]; int k = 0, j;
        while (i < n && (t[i] == '\r' || t[i] == '\n')) i++;
        while (i < n && t[i] != '\r' && t[i] != '\n') { if (k < 127) line[k++] = t[i]; i++; }
        line[k] = 0;
        // trim
        while (k > 0 && (line[k - 1] == ' ' || line[k - 1] == '\t')) line[--k] = 0;
        j = 0; while (line[j] == ' ' || line[j] == '\t') j++;
        if (!line[j] || line[j] == ';' || line[j] == '#' || line[j] == '[') continue;
        {
            char *key = line + j, *eq = strchr(key, '='), *end; long v;
            if (!eq) return 0;
            *eq = 0;
            { int kl = (int)strlen(key); while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) key[--kl] = 0; }
            eq++; while (*eq == ' ' || *eq == '\t') eq++;
            v = strtol(eq, &end, 10);
            if (end == eq || *end) return 0;
            if (d) {
                if (!strcmp(key, "capture")) d->capture = v != 0;
                else if (!strcmp(key, "mute")) d->mute = v != 0;
                else if (!strcmp(key, "trace")) d->trace = v != 0;
                continue;
            }
            if (!strcmp(key, "steamaudio")) s->steamaudio = clampi(v, 0, 1);
            else if (!strcmp(key, "reverb")) s->reverb = clampi(v, 0, 100);
            else if (!strcmp(key, "hrtf")) s->hrtf = clampi(v, 0, 1);
            else if (!strcmp(key, "env_room")) { s->env_room = clampi(v, 0, 3); s->has_env = 1; }
            else if (!strcmp(key, "env_wet")) { s->env_wet = clampi(v, 0, 100); s->has_env = 1; }
            else if (!strcmp(key, "env_speed")) { s->env_speed = clampi(v, 0, 1000); s->has_env = 1; }
            else if (!strcmp(key, "env_paused")) { s->env_paused = clampi(v, 0, 1); s->has_env = 1; }
            // other keys belong to other modules
        }
    }
    return 1;
}

// Reads the file into buf. Returns length, -1 when missing.
static int slurp(const wchar_t *path, char *buf, int cap) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!ReadFile(h, buf, cap - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    return (int)got;
}

static void apply(const SrSettings *s, const char *why) {
    int changed;
    EnterCriticalSection(&cs);
    changed = memcmp(&cur, s, sizeof *s) != 0;
    cur = *s;
    InterlockedExchange(&sa_on, s->steamaudio);
    LeaveCriticalSection(&cs);
    if (s->steamaudio && started > 1) fx_init();     // start loading Steam Audio before the next sound needs it
    if (changed && sr_dev)
        sr_log("settings (%s): steamaudio=%d reverb=%d hrtf=%d env=%d room=%d wet=%d speed=%d paused=%d", why,
               s->steamaudio, s->reverb, s->hrtf, s->has_env, s->env_room, s->env_wet, s->env_speed, s->env_paused);
}

static void read_ini(const char *why) {
    char buf[4096]; SrSettings s;
    int n = slurp(iniPath, buf, sizeof buf);
    if (n < 0) memset(&s, 0, sizeof s);
    else if (!parse(buf, n, &s, NULL)) { memset(&s, 0, sizeof s); if (sr_dev) sr_log("settings: parse error, all off"); }
    apply(&s, why);
}

static void read_dev(void) {
    char buf[1024]; SrDevOpts d; SrSettings dummy;
    int n = slurp(devPath, buf, sizeof buf);
    memset(&d, 0, sizeof d);
    if (n > 0) parse(buf, n, &dummy, &d);
    EnterCriticalSection(&cs);
    if (memcmp(&dev, &d, sizeof d)) { dev = d; if (sr_dev) sr_log("dev options: capture=%d mute=%d trace=%d", d.capture, d.mute, d.trace); }
    LeaveCriticalSection(&cs);
}

typedef struct { int exists; FILETIME t; DWORD size; } Stamp;
static Stamp stamp(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA a; Stamp s; memset(&s, 0, sizeof s);
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &a)) { s.exists = 1; s.t = a.ftLastWriteTime; s.size = a.nFileSizeLow; }
    return s;
}
static int same(Stamp a, Stamp b) { return a.exists == b.exists && a.size == b.size && !memcmp(&a.t, &b.t, sizeof a.t); }

static DWORD WINAPI poller(LPVOID x) {
    Stamp last = stamp(iniPath), lastDev = stamp(devPath);
    int missing = 0;
    (void)x;
    for (;;) {
        Stamp s;
        Sleep(250);
        s = stamp(iniPath);
        if (!same(s, last)) {
            // Lingo rewrites the whole file: a delete/create or a truncate can
            // be caught half way. A missing or empty file only counts once it
            // stays that way for two polls; a fresh write is read once it has
            // been still for 30 ms.
            if (!s.exists || s.size == 0) {
                if (++missing >= 2) { read_ini("removed"); last = s; missing = 0; }
            } else {
                Stamp again;
                Sleep(30);
                again = stamp(iniPath);
                if (same(again, s)) { read_ini("changed"); last = s; missing = 0; }
            }
        } else missing = 0;
        if (sr_dev) {
            Stamp d = stamp(devPath);
            if (!same(d, lastDev)) { lastDev = d; read_dev(); }
        }
    }
}

void sr_settings_start(void) {
    if (started) return;
    started = 1;
    InitializeCriticalSection(&cs);
    sr_exe_dir_filew(L"SunsetRacer2-fx.ini", iniPath, MAX_PATH);
    sr_exe_dir_filew(L"SunsetRacer2-audio-dev.txt", devPath, MAX_PATH);
    read_ini("startup");
    started = 2;
    if (sr_dev) read_dev();
    CloseHandle(CreateThread(NULL, 0, poller, NULL, 0, NULL));
}

void sr_settings_get(SrSettings *out) {
    if (!started) { memset(out, 0, sizeof *out); return; }
    EnterCriticalSection(&cs); *out = cur; LeaveCriticalSection(&cs);
}
void sr_devopts_get(SrDevOpts *out) {
    if (!started || !sr_dev) { memset(out, 0, sizeof *out); return; }
    EnterCriticalSection(&cs); *out = dev; LeaveCriticalSection(&cs);
}
int sr_steamaudio_on(void) { return (int)sa_on; }
