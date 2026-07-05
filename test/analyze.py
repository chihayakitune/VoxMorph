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
