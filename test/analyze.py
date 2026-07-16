#!/usr/bin/env python3
"""Verify PsolaEngine v0.2: f0, formants (LPC), intonation range,
breath (HF energy), consonant shift (spectral centroid), tilt."""
import wave
import numpy as np

FS = 48000

def load(path):
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return x.astype(np.float64) / 32768.0

def f0_autocorr(x, a=None, b=None):
    seg = x[a if a else len(x)//2 : b if b else len(x)//2 + 8192]
    seg = seg - seg.mean()
    ac = np.correlate(seg, seg, 'full')[len(seg)-1:]
    lo, hi = FS//500, FS//60
    lag = lo + np.argmax(ac[lo:hi])
    return FS / lag

def formants_lpc(x, order=16, fs_lp=12000):
    dec = FS // fs_lp
    t = np.arange(-64, 65)
    h = np.sinc(t / dec) / dec * np.hamming(129)
    y = np.convolve(x, h, 'same')[::dec]
    seg = y[len(y)//3 : len(y)//3 + 4096] * np.hamming(4096)
    seg = np.append(seg[0], seg[1:] - 0.97 * seg[:-1])
    r = np.correlate(seg, seg, 'full')[len(seg)-1:][:order+1]
    R = np.array([[r[abs(i-j)] for j in range(order)] for i in range(order)])
    a = np.linalg.solve(R + np.eye(order)*1e-9*r[0], -r[1:order+1])
    roots = np.roots(np.concatenate(([1.0], a)))
    roots = roots[np.imag(roots) > 0.01]
    freqs = np.angle(roots) * fs_lp / (2*np.pi)
    bws   = -np.log(np.abs(roots)) * fs_lp / np.pi
    return sorted(f for f, b in zip(freqs, bws) if 250 < f < 4000 and b < 500)[:3]

def band_ratio(x, lo, hi):
    seg = x[len(x)//3 : len(x)//3 + 16384] * np.hanning(16384)
    mag = np.abs(np.fft.rfft(seg))**2
    fr = np.fft.rfftfreq(16384, 1/FS)
    return mag[(fr>=lo)&(fr<hi)].sum() / mag.sum()

def centroid(x):
    seg = x[len(x)//3 : len(x)//3 + 16384] * np.hanning(16384)
    mag = np.abs(np.fft.rfft(seg))**2
    fr = np.fft.rfftfreq(16384, 1/FS)
    return (fr*mag).sum() / mag.sum()

def q(a, b, w=8192):  # f0 at two positions (first/second half)
    n = len(a and b or b)
    return 0

print("== basic (expect: same as v0.1) ==")
for path, desc in [
    ("out_dry.wav",     "dry            f0=120 F=700/1220/2600"),
    ("out_p0_f0.wav",   "identity       "),
    ("out_p7_f0.wav",   "pitch+7st      f0~180, F same"),
    ("out_p0_f7.wav",   "formant+7st    f0 120, F x1.50"),
    ("out_p12_fm4.wav", "p+12 f-4       f0~240"),
    ("out_robot.wav",   "robot 100Hz    f0~100"),
]:
    x = load(path)
    fmts = " ".join(f"{p:5.0f}" for p in formants_lpc(x))
    print(f"{desc}  f0={f0_autocorr(x):6.1f}  F={fmts}")

print("== pitch range (input: 110Hz then 132Hz, center=120) ==")
x1, x2 = load("out_range1.wav"), load("out_range2.wav")
n = len(x1)
for name, x in [("range=1.0", x1), ("range=2.0", x2)]:
    fa = f0_autocorr(x, n//4, n//4+8192)
    fb = f0_autocorr(x, 3*n//4, 3*n//4+8192)
    print(f"{name}: first={fa:6.1f} Hz  second={fb:6.1f} Hz   "
          f"(expect r1: 110/132, r2: ~100.8/~145.2)")

print("== breath (HF 4-10kHz energy ratio should rise) ==")
print(f"breath=0.0: {band_ratio(load('out_p0_f0.wav'), 4000, 10000):.4f}")
print(f"breath=0.9: {band_ratio(load('out_breath.wav'), 4000, 10000):.4f}")

print("== consonant shift (unvoiced, resonance 3kHz) ==")
c0, c7 = centroid(load("out_cons0.wav")), centroid(load("out_cons7.wav"))
print(f"cons 0st: centroid={c0:6.0f} Hz   cons+7st: {c7:6.0f} Hz  ratio={c7/c0:.2f} (expect ~1.5)")

print("== tilt +6dB (low/high balance) ==")
lo0 = band_ratio(load("out_p0_f0.wav"), 0, 1000);  hi0 = band_ratio(load("out_p0_f0.wav"), 2000, 8000)
lo6 = band_ratio(load("out_tilt6.wav"), 0, 1000);  hi6 = band_ratio(load("out_tilt6.wav"), 2000, 8000)
print(f"tilt 0: low={lo0:.3f} high={hi0:.3f}   tilt+6: low={lo6:.3f} high={hi6:.3f} (low up, high down)")

print("== air preserve v0.6 (breathy vowel 120Hz, pitch+7st) ==")
def hf_periodicity(x, f0_out, hp=3000):
    """Normalized autocorrelation of the >hp band at the OUTPUT pitch lag.
    High = aspiration was made periodic (metallic buzz); low = natural."""
    seg = x[len(x)//3 : len(x)//3 + 32768]
    X = np.fft.rfft(seg)
    fr = np.fft.rfftfreq(len(seg), 1/FS)
    X[fr < hp] = 0
    y = np.fft.irfft(X)
    y -= y.mean()
    lag = int(round(FS / f0_out))
    a, b = y[:-lag], y[lag:]
    return (a*b).sum() / np.sqrt((a*a).sum() * (b*b).sum() + 1e-30)

off, on = load("out_air_off.wav"), load("out_air_on.wav")
f0out = 120.0 * 2**(7/12)
dry_p = hf_periodicity(load("out_air_dry.wav"), f0out)
mx = load("out_air_max.wav")
print(f"HF periodicity @f0out lag: dry={dry_p:.3f}  off={hf_periodicity(off, f0out):.3f}  "
      f"on(1.0)={hf_periodicity(on, f0out):.3f}  max(1.5,band700)={hf_periodicity(mx, f0out):.3f}"
      f"   (on/max should approach dry)")
print(f"max setting: f0={f0_autocorr(mx):6.1f}  "
      f"HF 3-10k={band_ratio(mx, 3000, 10000):.4f}   (breath boosted ~2.6x expected)")
print(f"f0: off={f0_autocorr(off):6.1f}  on={f0_autocorr(on):6.1f}   (both ~{f0out:.0f})")
fo = " ".join(f"{p:5.0f}" for p in formants_lpc(off))
fn = " ".join(f"{p:5.0f}" for p in formants_lpc(on))
print(f"formants: off={fo}   on={fn}   (should match)")
ho, hn = band_ratio(off, 3000, 10000), band_ratio(on, 3000, 10000)
print(f"HF 3-10k energy ratio: off={ho:.4f}  on={hn:.4f}   "
      f"(similar = energy preserved; boost only engages above knob 1.0)")

# identity transparency: air=0.8 with no conversion must stay ~equal to air=0
i0, i1 = load("out_air_id0.wav"), load("out_air_id.wav")
n = min(len(i0), len(i1)); s = slice(n//3, n//3 + 32768)
d = i1[s] - i0[s]
rel = np.sqrt((d*d).mean() / max((i0[s]**2).mean(), 1e-30))
print(f"identity diff (air 1.0 vs 0, no conversion): rel RMS={rel:.3f}  "
      f"(mostly breath phase rearrangement, not a tonal change)")

print("== high range guard v0.8 (input 150->300Hz, pitch+7st, start 200Hz, amount 0%) ==")
ho_, hn_ = load("out_hi_off.wav"), load("out_hi_on.wav")
n = len(ho_)
for name, x in [("guard off", ho_), ("guard on ", hn_)]:
    fa = f0_autocorr(x, n//4, n//4 + 8192)
    fb = f0_autocorr(x, 3*n//4, 3*n//4 + 8192)
    print(f"{name}: low half={fa:6.1f} Hz (expect ~225 both)   "
          f"high half={fb:6.1f} Hz (off ~449, on ~355: guard tames the shift)")

print("== GCI grain sync v0.7 (pitch+7st) ==")
g = load("out_gci_vowel.wav")
fmts = " ".join(f"{p:5.0f}" for p in formants_lpc(g))
print(f"vowel gci=on: f0={f0_autocorr(g):6.1f} (expect ~179.8)  F={fmts} (expect ~= gci off)")
def periodicity(x, f0):
    seg = x[len(x)//3 : len(x)//3 + 16384]; seg = seg - seg.mean()
    ac = np.correlate(seg, seg, 'full')[len(seg)-1:]
    lag = int(round(FS / f0))
    return ac[max(1,lag-20):lag+20].max() / ac[0]
p_off = periodicity(load("out_p7_f0.wav"), 179.8)
p_on  = periodicity(g, 179.8)
print(f"full-band periodicity r(T): off={p_off:.4f}  on={p_on:.4f}   (>= off is good)")
co, cn = load("out_gci_creak_off.wav"), load("out_gci_creak_on.wav")
print(f"creaky+7st f0: off={f0_autocorr(co):5.1f}  on={f0_autocorr(cn):5.1f}  "
      f"r(T): off={periodicity(co, 82.4):.4f}  on={periodicity(cn, 82.4):.4f}"
      f"   (compare by listening too)")

print("== Natural Air v2 (band-adaptive comb) vs legacy Air Preserve ==")
def ghost_db(x, f0in, f0out, n=32768, at=None):
    """Energy on the ORIGINAL pitch's harmonic grid, excluding bins shared
    with the shifted grid: the audible 'old-pitch ghost'. Lower is better."""
    a = at if at is not None else len(x)//3
    seg = x[a : a+n] * np.hanning(n)
    mag = np.abs(np.fft.rfft(seg))**2
    total = mag.sum() + 1e-30
    outbins = set()
    for h in range(1, 400):
        b = int(round(h * f0out * n / FS))
        if b >= len(mag): break
        w = max(2, int(0.02 * b))
        outbins.update(range(b-2*w, b+2*w+1))
    e = 0.0
    for h in range(1, 400):
        b = int(round(h * f0in * n / FS))
        w = max(2, int(0.02 * b))
        if b + w >= len(mag): break
        if any(bb in outbins for bb in range(b-w, b+w+1)): continue
        e += mag[b-w:b+w+1].sum()
    return 10*np.log10(e/total + 1e-12)

f0i = 150.0
f0o = f0i * 2**(7/12)
print("old-pitch ghost after +7st (energy on the un-shifted harmonic grid):")
for name, base in [("steady harmonics", "out_nav2_harm"),
                   ("vibrato 5.5Hz   ", "out_nav2_vib"),
                   ("harm + white    ", "out_nav2_hw"),
                   ("harm + pink     ", "out_nav2_hp")]:
    gl = ghost_db(load(base + "_leg.wav"), f0i, f0o)
    gb = ghost_db(load(base + "_bac.wav"), f0i, f0o)
    print(f"  {name}: legacy={gl:6.1f} dB  v2={gb:6.1f} dB  (v2 lower = less leakage)")

print("known-noise retention (HF 4-16k energy ratio, want v2 ~= dry's noise):")
for name, base in [("white -15dB", "out_nav2_hw"), ("pink  -12dB", "out_nav2_hp")]:
    d = band_ratio(load(base + "_dry.wav"), 4000, 16000)
    l = band_ratio(load(base + "_leg.wav"), 4000, 16000)
    b = band_ratio(load(base + "_bac.wav"), 4000, 16000)
    print(f"  {name}: dry={d:.4f}  legacy={l:.4f}  v2={b:.4f}")

print("aspiration naturalness on the breathy vowel (HF periodicity @ output lag):")
bb  = load("out_nav2_breathy_bac.wav")
bl  = load("out_nav2_breathy_leg.wav")
f0o2 = 120.0 * 2**(7/12)
print(f"  dry={hf_periodicity(load('out_air_dry.wav'), f0o2):.3f}  off={hf_periodicity(off, f0o2):.3f}  "
      f"legacy={hf_periodicity(bl, f0o2):.3f}  v2={hf_periodicity(bb, f0o2):.3f}  (closer to dry = better)")
print(f"  f0: legacy={f0_autocorr(bl):6.1f}  v2={f0_autocorr(bb):6.1f}  (both ~{f0o2:.0f})")
fl_ = " ".join(f"{p:5.0f}" for p in formants_lpc(bl))
fb_ = " ".join(f"{p:5.0f}" for p in formants_lpc(bb))
print(f"  formants: legacy={fl_}   v2={fb_}   (should match)")

print("sibilant-only input (unvoiced: air path is gated, both should match):")
sd = band_ratio(load("out_nav2_sib_dry.wav"), 5000, 12000)
sl = band_ratio(load("out_nav2_sib_leg.wav"), 5000, 12000)
sb = band_ratio(load("out_nav2_sib_bac.wav"), 5000, 12000)
rl = np.sqrt((load("out_nav2_sib_leg.wav")**2).mean())
rb = np.sqrt((load("out_nav2_sib_bac.wav")**2).mean())
print(f"  5-12k ratio: dry={sd:.3f}  legacy={sl:.3f}  v2={sb:.3f}   RMS legacy={rl:.4f} v2={rb:.4f}")

print("low-pitch ghost, 90 Hz pulse rate +7st (subharmonic structure lives on")
print("the 45 Hz half-grid; energy there excluding the shifted grid = ghost):")
f0o90 = 90.0 * 2**(7/12)
for name, base in [("alternating +-1% ", "out_nav2_alt"),
                   ("subharmonic -15% ", "out_nav2_sub")]:
    gl = ghost_db(load(base + "_leg.wav"), 45.0, f0o90)
    gb = ghost_db(load(base + "_bac.wav"), 45.0, f0o90)
    print(f"  {name}: legacy={gl:6.1f} dB  v2={gb:6.1f} dB  (v2 <= legacy is good)")
g90 = ghost_db(load("out_nav2_low90_bac.wav"), 90.0, f0o90)
print(f"  90Hz steady, v2: old-grid energy={g90:6.1f} dB (compare across versions)")

print("Air Shine (top-band bypass gain only, breathy vowel +7st):")
for db, path in [(0, "out_nav2_breathy_bac.wav"),
                 (3, "out_nav2_shine3.wav"),
                 (6, "out_nav2_shine6.wav")]:
    x = load(path)
    print(f"  +{db} dB: 6-16k={band_ratio(x,6000,16000):.5f}  1-4k={band_ratio(x,1000,4000):.4f}"
          f"  rms={np.sqrt((x*x).mean()):.4f}   (6-16k up, 1-4k & rms ~flat)")

print("control-rate settling after unvoiced->voiced (HF air onset, evaluates")
print("the 512-sample update + 5 ms gain smoothing; voiced starts at t=1.0s):")
def onset_ms(path):
    x = load(path)
    X = np.fft.rfft(x)
    fr = np.fft.rfftfreq(len(x), 1/FS)
    X[(fr < 3000) | (fr > 10000)] = 0
    y = np.fft.irfft(X, len(x))
    hop = int(0.005 * FS)
    env = np.array([np.sqrt((y[i:i+hop]**2).mean()) for i in range(0, len(y)-hop, hop)])
    t = np.arange(len(env)) * hop / FS
    steady = np.median(env[(t > 1.6) & (t < 2.6)])
    after = np.where((t > 1.0) & (env > 0.8 * steady))[0]
    return (t[after[0]] - 1.0) * 1000 if len(after) else float('nan')
print(f"  time to 80% of steady HF: legacy={onset_ms('out_nav2_trans_leg.wav'):.0f} ms  "
      f"v2={onset_ms('out_nav2_trans_bac.wav'):.0f} ms  (v2 - legacy = added tracking lag)")
