# sr2audio: Steam Audio for Sunset Racer 2

`dsound.dll` sits beside the game exe. Director's DirectSound Xtra loads it in
place of the System32 one. It forwards every export to the real `dsound.dll`.
With Steam Audio switched on, it takes over the game's sound mixing and adds
room reverb, rendered by Steam Audio 4.8.1 (`phonon.dll`, x86).

## Toggles (SunsetRacer2-fx.ini, beside the exe)

| key | values | effect |
|---|---|---|
| `steamaudio` | 0 / 1 | 0: every sound buffer is the game's own DirectSound buffer, untouched (exact pass-through). 1: the proxy mixes the game's sounds itself |
| `reverb` | 0..100 | reverb level (percent of full) |
| `hrtf` | 0 / 1 | headphone 3D for sounds the game positions (see "Lingo") |
| `env_room` | 0..3 | live, races only: 0 open, 1 street, 2 underpass, 3 tunnel |
| `env_wet` | 0..100 | live: wet road; damps the treble of the reverb (-10 dB at 3 kHz+) and a little of the direct sound (-4 dB) |
| `env_paused` | 0 / 1 | live: 1 fades the reverb out in 60 ms and drops the tail |
| `env_speed` | km/h | read but not used yet |

- A missing file or key means 0. A line that does not parse turns everything off.
- A background thread checks the file's write time every 250 ms. The game thread never touches the file.
- A missing or empty file only counts after two polls. A changed file is read once it has stayed unchanged for 30 ms, which covers half-written files.
- With no `env_` key in the file (the menus), no new reverb starts. Tails that are already ringing fade out naturally.
- A fresh install has no ini, so everything is off and the game sounds exactly like the original.

## Design

- **Interception.** The proxy patches `IDirectSound::CreateSoundBuffer`, `DuplicateSoundBuffer` and `Release` on the device the Xtra creates.
  - When `steamaudio=1` and Steam Audio has loaded, each PCM 8/16-bit mono or stereo secondary buffer becomes a virtual `IDirectSoundBuffer8` with an `IDirectSoundNotify` interface.
  - Everything else goes to real DirectSound: primary, 3D and FX buffers, other formats, and every buffer while the mode is off.
- **Virtual buffers.** They behave like real DirectSound (compared call by call with `sr2audio-test semantics`):
  - Lock and Unlock with wrap, `FROMWRITECURSOR` and `ENTIREBUFFER`.
  - Play and loop; a non-looping buffer stops at its end with status 0 and position 0.
  - Volume, pan and frequency, with the same ranges and error codes as real DirectSound.
  - Position notifications. `DSBPN_OFFSETSTOP` fires on Stop only when the buffer was playing.
  - `SetNotificationPositions` is refused while the buffer plays.
  - The play cursor advances at the mixer's rate. The write cursor is one block plus 16 frames ahead of it.
- **Mixer thread.**
  - Runs under MMCSS "Pro Audio", off the game thread.
  - Clocked by the play cursor of one real output buffer: 48 kHz stereo float with a 16-bit fallback, a 250 ms ring and 10 ms blocks.
  - The resampler is a 16-tap Kaiser-windowed sinc with 256 phases, written in SSE.
  - Volume and pan follow DirectSound's law and are ramped over each block.
- **Latency.** About 50 ms from buffer to output (write gap plus 1.5 blocks). The game's own buffers sit about 30 ms behind the cursor, so the proxy adds roughly 20 ms. With `steamaudio=0` it adds nothing.
- **Reverb.**
  - Each room is a small scene that Steam Audio's ray tracer simulates when the proxy loads (16k rays). The rooms are:

    | room | geometry |
    |---|---|
    | open | ground plus distant facades |
    | street | a canyon 16 m wide and 18 m high |
    | underpass | a concrete deck 6 m up |
    | tunnel | a tiled tube 11×6.5 m and 600 m long |

  - The rendering is HYBRID: a 0.1 s convolution head and a parametric tail, in first-order ambisonics, decoded to stereo by panning.
  - The proxy scales the simulated reverb times down to a cap per room and keeps the ratios between frequency bands. The caps are open 0.6 s, street 0.9 s, underpass 1.5 s and tunnel 3 s.
  - Mono sources (engine, skids, hits) feed the reverb. Stereo sources (music, ambience) stay dry.
  - A room change crossfades the reverb input over 150 ms. Old tails ring out.
  - A room counts as idle once its output stays below -96 dB for 10 blocks. Idle rooms cost nothing.
  - Reflection effects are never reset, because a reset loses their impulse response. They are drained instead.
  - A soft clip above -1 dBFS runs only while reverb is active.
- **Steam Audio loading.** `phonon.dll` is delay-loaded and loads on a worker thread when `steamaudio=1` is first seen. Sounds stay on real DirectSound until it is ready, which takes about 1-2 s, so there is no hitch on the game thread.

## Measurements

These numbers come from an i7-8550U laptop, with the game running at 1080p in the same process.

| test | result |
|---|---|
| direct sound path, 1 kHz sine at -600 mB | level exact (0.2506), THD+N -87 dB |
| 48 kHz stereo | peak 0.49997 for a 0.5 source, THD+N -91 dB |
| harness: wet/dry energy of a white burst | open -15, street -9.4, underpass -5.3, tunnel -3.3 dB |
| harness: tail T30 | open 0.05, street 1.0, underpass 1.6, tunnel 3.1 s |
| in race: wet/dry, engine and skids | open -11, street -7, underpass -1, tunnel 0..+1 dB |
| `reverb=50` | -6 dB wet |
| `env_wet=100` | HF/LF balance of the tunnel tail from +8.5 to -0.6 dB |
| `env_paused` | reverb silent within 470 ms of the write (poll latency included); in race exact silence while paused, no click |
| room switches | no added clicks (2nd-difference peak: mix 6.6 dB vs dry 5.4 dB) |
| HRTF | pan right +16.8 dB R-L, left -15.6 dB, behind -1 dB |
| dropouts / underruns | 0 in all runs (61 s in-race capture, 3048 windows of 20 ms) |
| CPU per 10 ms block, mixer thread, in race (5 voices) | 0.68-0.92 ms avg (mix 0.15, Steam Audio 0.52-0.75); spikes up to 4.4 ms are absorbed by the 50 ms lead |
| CPU in menus or with no env keys | 0.03-0.15 ms avg |

### Test commands

```
powershell -NoProfile -ExecutionPolicy Bypass -File native\sr2audio\build.ps1 [-Install <game folder>]
out\sr2audio-test.exe semantics real|proxy [trace]   # API behaviour, real DirectSound vs proxy (silent)
out\sr2audio-test.exe render                        # scripted scenario, writes captures (silent: mute=1)
out\sr2audio-test.exe quick 0|1
python test\analyze.py <folder>                     # IR and scenario report
python test\analyze.py <folder> --game              # in-game capture: wet/dry per second, clicks, dropouts
```

- Dev outputs only appear when `SunsetRacer2-dev.txt` exists beside the exe:
  - `SunsetRacer2-audio.log` gets per-5-second CPU, voice, underrun and room statistics.
  - `SunsetRacer2-audio-dev.txt` can hold `capture=1` (writes `SunsetRacer2-audio-mix-NNN.wav` and `-dry-NNN.wav`, 32-bit float), `mute=1` (silent output) and `trace=1` (logs API calls).
- `render` also writes `SunsetRacer2-audio-ir-<room>.wav`, the impulse response of each room.
- Use plain `python`: numpy lives in the user site, so `-I` breaks it.

## Build and ship

- The build needs MSVC 2022 Build Tools (x86) and the Steam Audio 4.8.1 SDK unpacked at `third_party\steamaudio\steamaudio`. `third_party\` is not committed.
- Ship `dsound.dll` and `phonon.dll` (46 MB) beside `SunsetRacer2.exe`. `build.ps1 -Install <folder>` copies both.
- Without `phonon.dll` the proxy stays a pure pass-through.

## Known gaps

- Turning `steamaudio` on or off only affects sounds created afterwards. A sound that is already playing (for example a looping engine or the music) keeps its path until the game recreates it. After turning the mode off, those voices still play dry through the proxy, with no reverb, until they are recreated.
- `hrtf=1` only applies to mono sounds that carry a pan value (see below). Everything else is unchanged. At most 4 sounds are rendered binaurally at once.
- `env_speed` is read but unused.
- The pass-through path cannot be captured, because it is the game's own DirectSound. It is verified by buffer counts in the log (`buffers made real N virtual 0`).
- In-race CPU is just under 1 ms per block, which is more than the target of "well under". The reverb (Steam Audio's hybrid effect) accounts for 0.5-0.75 ms of it. Setting `SR_RTYPE` to parametric at build time roughly halves that, but loses the 0.1 s convolution head.

## Lingo

- Nothing beyond the shared contract is required.
- Write `steamaudio`, `reverb` and `hrtf` from the settings page.
- During races, write `env_room`, `env_wet`, `env_speed` and `env_paused` at about 5 Hz.
- Leave the `env_` keys out of the file outside races.
- Optional, for `hrtf=1`: a mono sound with a non-zero pan is placed at azimuth `pan × 180 / 10000` degrees. In DirectSound units, 0 is ahead and ±10000 is behind.
  - To position a sound, set `sound(ch).pan = azimuthDeg / 1.8` (range -100..100) only while `steamaudio=1` and `hrtf=1`.
  - It has not been checked how Director scales Lingo pan into `SetPan`. Confirm it with `trace=1`.
