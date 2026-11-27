// sr2audio: dsound.dll proxy for Sunset Racer 2 (32-bit).
//
// steamaudio=0 (or no settings file): DirectSoundCreate hands back the real
// System32 device. The only change is one patched vtable slot on the device
// (CreateSoundBuffer), which forwards the call untouched and returns the real
// buffer, so Director writes straight into real DirectSound memory.
//
// steamaudio=1: new secondary buffers are virtual (vbuf.c). A mixer thread
// (mixer.c) plays them the way DirectSound would (cursors, loops, volume, pan,
// frequency, notifications), mixes them, runs the Steam Audio chain (fx.c) and
// writes the result into one real DirectSound buffer.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#define SR_RATE   48000          // mixer and output rate
#define SR_BLOCK  480            // frames per mixer block (10 ms)
#define SR_NBIN   4              // voices that can be rendered binaurally at once

// ---- settings (settings.c) -------------------------------------------------
typedef struct {
    int steamaudio, reverb, hrtf;
    int has_env;                 // the live section is present (a race is running)
    int env_room, env_wet, env_speed, env_paused;
} SrSettings;

void  sr_settings_start(void);           // first synchronous read + poll thread
void  sr_settings_get(SrSettings *out);  // lock-protected copy, no IO
int   sr_steamaudio_on(void);            // cheap, for CreateSoundBuffer

// dev options (SunsetRacer2-audio-dev.txt, read only when the dev file exists)
typedef struct { int capture, mute, trace; } SrDevOpts;
void  sr_devopts_get(SrDevOpts *out);

// ---- logging (proxy.c) -----------------------------------------------------
extern int sr_dev;                       // SunsetRacer2-dev.txt exists
void  sr_log(const char *fmt, ...);
void  sr_exe_dir_file(const char *name, char *out, int cap);
void  sr_exe_dir_filew(const wchar_t *name, wchar_t *out, int cap);

// ---- virtual buffers (vbuf.c) ----------------------------------------------
typedef struct VData { LONG ref; BYTE *mem; DWORD bytes; } VData;

typedef struct VBuf {
    const void *lpVtbl;          // IDirectSoundBuffer8 vtable (must be first)
    const void *lpNotifyVtbl;    // IDirectSoundNotify sub-object
    LONG ref;
    int id;
    VData *data;
    WAVEFORMATEX wfx;
    DWORD flags, bytes, frames;
    int ch, bits, align;
    LONG vol, pan;
    DWORD freq;                  // 0 = original rate
    int playing, looping;
    double pos;                  // play position in frames, [0, frames)
    DSBPOSITIONNOTIFY *notes;
    int nnotes;
    // mixer state
    float gl, gr;                // gains used at the end of the last block
    int fresh;                   // no block mixed since Play / SetCurrentPosition
    float hist[2 * 16];          // last 16 source frames consumed (interpolator history)
    int bslot;                   // binaural slot or -1
    struct VBuf *next, *prev;
} VBuf;

HRESULT vbuf_create(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out);
HRESULT vbuf_duplicate(VBuf *src, LPDIRECTSOUNDBUFFER *out);
int     vbuf_is(void *p);
DWORD   vbuf_read_margin(VBuf *v);   // frames the mixer reads past pos per block

// ---- mixer (mixer.c) -------------------------------------------------------
extern CRITICAL_SECTION g_mix;   // guards the voice list and every VBuf field
extern VBuf *g_voices;
void  mixer_attach_device(IDirectSound *ds, void *realCSB);
void  mixer_wake(void);          // a voice started: make sure the thread runs
void  mixer_voice_removed(VBuf *v);
IDirectSound *mixer_device(void);
void  mixer_shutdown(void (*release_dev)(IDirectSound *));

// ---- effects (fx.c) --------------------------------------------------------
typedef struct {
    float *dryL, *dryR;          // non-diegetic (stereo sources): untouched
    float *dieL, *dieR;          // diegetic dry (mono sources)
    float *send;                 // mono reverb send
    float *bin[SR_NBIN];         // mono signal of each binaural slot
    int    binUsed[SR_NBIN];     // slot carried a voice this block
    float  binAz[SR_NBIN];       // azimuth in degrees, 0 ahead, + right, 180 behind
} SrBus;

int   fx_init(void);             // loads phonon.dll, starts building rooms; 0 = unavailable
void  fx_process(SrBus *b, const SrSettings *s, float *outL, float *outR);
int   fx_ready(void);            // Steam Audio loaded: new buffers may go virtual
int   fx_busy(void);             // reverb tail still sounding
void  fx_stats(char *buf, int cap);
