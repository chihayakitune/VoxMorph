#!/usr/bin/env python3
"""Verify PsolaEngine output: f0 via autocorrelation, formants via LPC roots."""
import wave
import numpy as np

FS = 48000

def load(path):
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return x.astype(np.float64) / 32768.0

def f0_autocorr(x):
    seg = x[len(x)//2 : len(x)//2 + 8192]
    seg = seg - seg.mean()
    ac = np.correlate(seg, seg, 'full')[len(seg)-1:]
    lo, hi = FS//500, FS//60
    lag = lo + np.argmax(ac[lo:hi])
    return FS / lag

def formants_lpc(x, order=12, fs_lp=12000):
    # decimate 48k -> 12k with a windowed-sinc lowpass
    dec = FS // fs_lp
    t = np.arange(-64, 65)
    h = np.sinc(t / dec) / dec * np.hamming(129)
    y = np.convolve(x, h, 'same')[::dec]
    seg = y[len(y)//3 : len(y)//3 + 4096]
    seg = seg * np.hamming(len(seg))
    seg = np.append(seg[0], seg[1:] - 0.97 * seg[:-1])   # pre-emphasis
    # autocorrelation method
    r = np.correlate(seg, seg, 'full')[len(seg)-1:][:order+1]
    R = np.array([[r[abs(i-j)] for j in range(order)] for i in range(order)])
    a = np.linalg.solve(R + np.eye(order)*1e-9*r[0], -r[1:order+1])
    poly = np.concatenate(([1.0], a))
    roots = np.roots(poly)
    roots = roots[np.imag(roots) > 0.01]
    freqs = np.angle(roots) * fs_lp / (2*np.pi)
    bws   = -np.log(np.abs(roots)) * fs_lp / np.pi
    sel = sorted(f for f, b in zip(freqs, bws) if 250 < f < 4000 and b < 500)
    return sel[:3]

cases = [
    ("out_dry.wav",     "dry (f0=120, F=700/1220/2600)          "),
    ("out_p0_f0.wav",   "pitch 0st  formant 0st  (expect: same) "),
    ("out_p7_f0.wav",   "pitch+7st  formant 0st  (f0~180 F same)"),
    ("out_p0_f7.wav",   "pitch 0st  formant+7st  (f0 120 Fx1.50)"),
    ("out_p0_fm5.wav",  "pitch 0st  formant-5st  (f0 120 Fx0.75)"),
    ("out_p12_fm4.wav", "pitch+12st formant-4st  (f0~240 Fx0.79)"),
    ("out_robot.wav",   "robot 100Hz             (f0~100 F same)"),
]
for path, desc in cases:
    x = load(path)
    fmts = " ".join(f"{p:6.0f}" for p in formants_lpc(x))
    print(f"{desc}  f0={f0_autocorr(x):6.1f}  F={fmts}")
