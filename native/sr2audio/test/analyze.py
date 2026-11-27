"""Measures the sr2audio test captures.

    python -I analyze.py <folder>            # impulse responses + the harness render scenario
    python -I analyze.py <folder> --game     # a capture taken in the game (levels, clicks, CPU from the log)

Reads SunsetRacer2-audio-ir-*.wav, SunsetRacer2-audio-mix-NNN.wav and
SunsetRacer2-audio-dry-NNN.wav (32-bit float stereo, 48 kHz) written by the
proxy when SunsetRacer2-audio-dev.txt holds capture=1.
"""
import glob
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt

RATE = 48000


def load(path):
    r, x = wavfile.read(path)
    assert r == RATE, path
    return x.astype(np.float64)


def band(x, lo, hi):
    sos = butter(4, [lo, hi], btype="band", fs=RATE, output="sos")
    return sosfilt(sos, x)


def schroeder_rt(e):
    """RT60 from a squared, summed impulse response: EDT, T20, T30 (seconds)."""
    s = np.cumsum(e[::-1])[::-1]
    if s[0] <= 0:
        return (float("nan"),) * 3
    d = 10 * np.log10(np.maximum(s / s[0], 1e-30))
    t = np.arange(len(d)) / RATE

    def fit(a, b):
        idx = np.where((d <= a) & (d >= b))[0]
        if len(idx) < 10:
            return float("nan")
        k = np.polyfit(t[idx], d[idx], 1)[0]
        return -60.0 / k if k < 0 else float("nan")

    return fit(0, -10), fit(-5, -25), fit(-5, -35)


def ir_report(folder):
    print("== impulse responses (decoded to stereo, as heard at reverb=100) ==")
    print(f"{'room':10s} {'energy dB':>9s} {'EDT':>6s} {'T20':>6s} {'T30':>6s}   T30 by band 250 / 1k / 4k Hz      first echo")
    rows = []
    for p in sorted(glob.glob(os.path.join(folder, "SunsetRacer2-audio-ir-*.wav"))):
        x = load(p)
        name = os.path.basename(p)[len("SunsetRacer2-audio-ir-"):-4]
        e = (x ** 2).sum(axis=1)
        en = 10 * np.log10(e.sum() / 2 + 1e-30)          # per channel, relative to the unit dry impulse
        edt, t20, t30 = schroeder_rt(e)
        bands = []
        for lo, hi in ((177, 354), (707, 1414), (2828, 5657)):
            b = band(x, lo, hi)
            bands.append(schroeder_rt((b ** 2).sum(axis=1))[2])
        first = np.argmax(np.abs(x).max(axis=1) > np.abs(x).max() * 0.05) / RATE * 1000
        rows.append((name, en, t30))
        print(f"{name:10s} {en:9.1f} {edt:6.2f} {t20:6.2f} {t30:6.2f}   {bands[0]:5.2f} / {bands[1]:5.2f} / {bands[2]:5.2f}"
              f"            {first:5.0f} ms")
    return rows


def env_rms(x, win=480):
    n = len(x) // win
    return np.sqrt((x[: n * win] ** 2).reshape(n, win, -1).mean(axis=(1, 2)))


def regions(dry, thresh=1e-3, gap=4):
    """Contiguous stretches of dry signal (10 ms resolution), merging gaps under 40 ms."""
    r = env_rms(dry) > thresh
    out, start, quiet = [], None, 0
    for i, v in enumerate(r):
        if v:
            if start is None:
                start = i
            quiet = 0
        elif start is not None:
            quiet += 1
            if quiet > gap:
                out.append((start, i - quiet + 1))
                start, quiet = None, 0
    if start is not None:
        out.append((start, len(r)))
    return [(a * 480, b * 480) for a, b in out]


def thd_n(x, f0):
    w = np.hanning(len(x))
    X = np.abs(np.fft.rfft(x * w)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    k = np.argmin(np.abs(f - f0))
    sig = X[k - 3: k + 4].sum()
    noise = X[(f > 20) & (f < 20000)].sum() - sig
    return 10 * np.log10(noise / sig)


def clicks(x):
    """Largest jump of the 2nd difference relative to the signal's local RMS (dB); a click shows as a high value."""
    m = x.mean(axis=1) if x.ndim > 1 else x
    d2 = np.abs(np.diff(m, 2))
    local = np.sqrt(np.convolve(m ** 2, np.ones(480) / 480, mode="same"))[1:-1] + 1e-6
    return 20 * np.log10((d2 / local).max())


def scenario(folder):
    mixes = sorted(glob.glob(os.path.join(folder, "SunsetRacer2-audio-mix-*.wav")))
    if not mixes:
        print("no captures")
        return
    mix = load(mixes[-1])
    dry = load(mixes[-1].replace("-mix-", "-dry-"))
    n = min(len(mix), len(dry))
    mix, dry = mix[:n], dry[:n]
    wet = mix - dry
    reg = regions(dry)
    print(f"\n== render scenario ({os.path.basename(mixes[-1])}, {n / RATE:.1f} s, {len(reg)} sounds found) ==")
    names = ["1 kHz sine, -6 dB, no race", "stereo 500/700 Hz 48 kHz", "burst open", "burst street", "burst underpass",
             "burst tunnel", "sweep, rooms switching", "burst tunnel + pause", "burst tunnel env_wet=100",
             "hrtf pan right", "hrtf pan left", "hrtf behind", "burst tunnel reverb=50", "burst tunnel hrtf=1"]
    if len(reg) != len(names):
        print("unexpected number of sounds:", [(a / RATE, b / RATE) for a, b in reg])
    res = {}
    for i, (a, b) in enumerate(reg[: len(names)]):
        res[names[i]] = (a, b)

    a, b = res[names[0]]
    seg = mix[a + 4800: b - 4800, 0]
    print(f"sine: peak {np.abs(seg).max():.4f} (source 0.5 at -600 mB -> {0.5 * 10 ** (-600 / 2000):.4f}),"
          f" THD+N {thd_n(seg, 1000):.1f} dB, wet energy {10 * np.log10((wet[a:b] ** 2).mean() + 1e-30):.1f} dB (no race: dry only)")
    a, b = res[names[1]]
    seg = mix[a + 4800: b - 4800]
    t = np.arange(len(seg))
    # bit-exact path: compare against the 16-bit source the harness made
    print(f"stereo 48 kHz: peaks L {np.abs(seg[:, 0]).max():.5f} R {np.abs(seg[:, 1]).max():.5f} (source 0.5),"
          f" THD+N L {thd_n(seg[:, 0], 500):.1f} dB R {thd_n(seg[:, 1], 700):.1f} dB")

    print(f"{'burst':28s} {'wet/dry dB':>10s} {'tail T30':>8s} {'HF/LF wet dB':>12s} {'clicks dB':>9s}")
    for nm in names[2:6] + names[7:9] + names[12:14]:
        a, b = res[nm]
        tail_end = min(n, b + int(3.5 * RATE))
        wd = 10 * np.log10((wet[a:tail_end] ** 2).sum() / max((dry[a:b] ** 2).sum(), 1e-30))
        e = (wet[b:tail_end] ** 2).sum(axis=1)
        t30 = schroeder_rt(e)[2]
        tail = wet[b + int(0.25 * RATE): b + int(1.5 * RATE), 0]       # pure reverb, no dry
        hf = (band(tail, 4000, 10000) ** 2).sum()
        lf = (band(tail, 200, 1000) ** 2).sum()
        print(f"{nm:28s} {wd:10.1f} {t30:8.2f} {10 * np.log10(hf / lf + 1e-30):12.1f} {clicks(mix[a:tail_end]):9.1f}")

    # the pause was written 300 ms after the burst; the proxy sees it within 250-330 ms more
    a3, _ = res[names[5]]
    a, b = res[names[7]]
    w1 = slice(a + int(0.25 * RATE), a + int(0.30 * RATE))
    w2 = slice(a + int(0.75 * RATE), a + int(2.0 * RATE))
    lvl = lambda x: 20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-12)
    print(f"pause: wet RMS 250-300 ms {lvl(wet[w1]):.1f} dB (unpaused run {lvl(wet[a3 + int(0.25 * RATE): a3 + int(0.30 * RATE)]):.1f}),"
          f" 0.75-2 s {lvl(wet[w2]):.1f} dB (unpaused run {lvl(wet[a3 + int(0.75 * RATE): a3 + int(2.0 * RATE)]):.1f})")
    z = np.where(env_rms(wet[a:a + 2 * RATE]) > 0)[0]
    print(f"pause: last non-silent wet block {z.max() * 10 if len(z) else 0} ms after the burst")

    a, b = res[names[6]]
    print(f"sweep with room switches: clicks mix {clicks(mix[a:b]):.1f} dB vs dry {clicks(dry[a:b]):.1f} dB"
          f" (2nd-difference peak over local RMS; similar = no added clicks)")
    for nm in names[9:12]:
        a, b = res[nm]
        l, r = (mix[a:b] ** 2).sum(axis=0)
        print(f"{nm:16s}: right minus left {10 * np.log10(r / l):+.1f} dB")


def game(folder):
    mixes = sorted(glob.glob(os.path.join(folder, "SunsetRacer2-audio-mix-*.wav")))
    for m in mixes:
        mix = load(m)
        d = m.replace("-mix-", "-dry-")
        dry = load(d) if os.path.exists(d) else None
        print(f"\n{os.path.basename(m)}: {len(mix) / RATE:.1f} s, peak {np.abs(mix).max():.3f}, RMS {20 * np.log10(np.sqrt((mix ** 2).mean()) + 1e-12):.1f} dB")
        if dry is not None:
            n = min(len(mix), len(dry))
            wet = mix[:n] - dry[:n]
            print(f"  wet/dry energy {10 * np.log10((wet ** 2).sum() / max((dry[:n] ** 2).sum(), 1e-30)):.1f} dB,"
                  f" clicks mix {clicks(mix[:n]):.1f} dB vs dry {clicks(dry[:n]):.1f} dB")
            # per second wet/dry, to see room changes
            sec = [10 * np.log10((wet[i:i + RATE] ** 2).sum() / max((dry[i:i + RATE] ** 2).sum(), 1e-30)) for i in range(0, n - RATE, RATE)]
            print("  wet/dry per second:", " ".join(f"{v:.0f}" for v in sec))
        # dropouts: 20 ms of exact zeros in the middle of sound
        z = env_rms(mix, 960) == 0
        print(f"  silent 20 ms windows: {int(z.sum())} of {len(z)}")


if __name__ == "__main__":
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    if "--game" in sys.argv:
        game(folder)
    else:
        ir_report(folder)
        scenario(folder)
