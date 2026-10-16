#!/usr/bin/env python3
"""Offline report for motor excitation logs (DEBUG_MOTOR_EXCITATION + blackbox MOTOR_TEST).

For every session in the log: input u = excitation signal (debug[0], command units), outputs = raw gyro
(gyroUnfilt, deg/s) and acc. Welch PSD, transfer function H = Pyu/Puu, coherence, and a peak table
(frequency, -3 dB bandwidth, Q, gain, coherence). Measured quantities only; no model.

  bbl_report.py logs/x.bbl [--out logs/x_report] [--nperseg 1024]
  bbl_report.py --selftest            synthetic resonator, checks the peak finder
"""
import argparse
import csv
import os
import sys

import numpy as np
from scipy import signal

MIN_COHERENCE = 0.6        # peaks below this are not reported as identified
STATE_RUN = 2


def transfer(u, y, fs, nperseg):
    f, puu = signal.welch(u, fs, nperseg=nperseg)
    _, pyu = signal.csd(u, y, fs, nperseg=nperseg)
    _, coh = signal.coherence(u, y, fs, nperseg=nperseg)
    _, pyy = signal.welch(y, fs, nperseg=nperseg)
    with np.errstate(divide="ignore", invalid="ignore"):
        h = pyu / puu
    return f, h, coh, puu, pyy


def peaks(f, h, coh, fmin=10.0, min_coh=MIN_COHERENCE, max_peaks=6):
    """Resonance peaks of |H| where the coherence is high enough. Returns list of dicts."""
    mag = np.abs(h)
    ok = (f >= fmin) & (coh >= min_coh) & np.isfinite(mag)
    m = np.where(ok, mag, 0.0)
    if not ok.any():
        return []
    idx, _ = signal.find_peaks(m, prominence=0.2 * m.max())
    out = []
    for i in sorted(idx, key=lambda k: -m[k])[:max_peaks]:
        half = m[i] / np.sqrt(2)
        lo = i
        while lo > 0 and m[lo] > half:
            lo -= 1
        hi = i
        while hi < len(m) - 1 and m[hi] > half:
            hi += 1
        # linear interpolation of the -3 dB crossings
        flo = np.interp(half, [m[lo], m[lo + 1]], [f[lo], f[lo + 1]]) if lo < i else f[i]
        fhi = np.interp(half, [m[hi], m[hi - 1]], [f[hi], f[hi - 1]]) if hi > i else f[i]
        bw = max(fhi - flo, f[1] - f[0])
        out.append({"frequency_hz": float(f[i]), "bandwidth_hz": float(bw), "q": float(f[i] / bw),
                    "gain": float(m[i]), "coherence": float(coh[i])})
    return sorted(out, key=lambda p: p["frequency_hz"])


def selftest():
    rng = np.random.default_rng(1)
    fs, n = 2000.0, 2000 * 20
    u = rng.standard_normal(n) * 35.0
    f0, q = 180.0, 8.0
    b, a = signal.iirpeak(f0, q, fs)               # resonator, unit gain at f0
    y = 0.5 * signal.lfilter(b, a, u) + rng.standard_normal(n) * 2.0
    f, h, coh, _, _ = transfer(u, y, fs, 1024)
    p = peaks(f, h, coh)
    print("synthetic peaks:", p)
    good = len(p) >= 1 and abs(p[0]["frequency_hz"] - f0) < 4 and 5 < p[0]["q"] < 12 and abs(p[0]["gain"] - 0.5) < 0.1
    # repeatability: a second independent record gives a close estimate
    u2 = rng.standard_normal(n) * 35.0
    y2 = 0.5 * signal.lfilter(b, a, u2) + rng.standard_normal(n) * 2.0
    p2 = peaks(*transfer(u2, y2, fs, 1024)[:3])
    rep = p2 and abs(p2[0]["frequency_hz"] - p[0]["frequency_hz"]) < 4
    # no excitation -> no identified peak
    p3 = peaks(*transfer(np.zeros(n) + 1e-9 * rng.standard_normal(n), rng.standard_normal(n), fs, 1024)[:3])
    print("repeat:", p2[:1], "noise-only peaks:", p3)
    ok = good and rep and not p3
    print("SELFTEST", "PASS" if ok else "FAIL")
    return ok


def load(path, log_index=None):
    """All logs in the file concatenated (every MOTOR_TEST start opens a new log). Returns names, data, header of
    the first log; per-frame session identity comes from the debug channels."""
    from orangebox import Parser
    p = Parser.load(path)
    names, hdr, rows = p.field_names, p.headers, []
    indices = [log_index] if log_index else range(1, p.reader.log_count + 1)
    for li in indices:
        q = Parser.load(path, li)
        if q.field_names != names:
            print(f"log {li}: different fields, skipped"); continue
        for fr in q.frames():
            if len(fr.data) == len(names):
                rows.append([float(x) if x not in ("", None) else np.nan for x in fr.data])
    return names, np.array(rows, dtype=float), hdr


def col(names, data, name):
    return data[:, names.index(name)] if name in names else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bbl", nargs="?")
    ap.add_argument("--out", default=None)
    ap.add_argument("--nperseg", type=int, default=1024)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)

    names, d, hdr = load(a.bbl)
    out = a.out or os.path.splitext(a.bbl)[0] + "_report"
    hires = int(hdr.get("blackbox_high_resolution", 0) or 0)
    gscale = 0.1 if hires else 1.0
    t = col(names, d, "time") * 1e-6
    u = col(names, d, "debug[0]")
    state = (col(names, d, "debug[3]").astype(int) >> 4)
    sess = col(names, d, "debug[4]").astype(int)
    gyro = [col(names, d, f"gyroUnfilt[{k}]") * gscale for k in range(3)]
    acc = [col(names, d, f"accSmooth[{k}]") for k in range(3)]
    exc = hdr.get("motor_excitation", "")
    print(f"{a.bbl}: {len(t)} frames, {t[-1] - t[0]:.1f} s, header motor_excitation={exc}, high_res={hires}")

    rows = []
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    for s in sorted(set(sess[state == STATE_RUN])):
        sel = (sess == s) & (state == STATE_RUN)
        if sel.sum() < 4 * a.nperseg:
            print(f"session {s}: {sel.sum()} frames, too short"); continue
        # resample onto a uniform grid by the real timestamps: loop jitter and missed loops otherwise scale
        # the frequency axis (input and response were both sampled at the same real instants)
        ts = t[sel]
        fs = (len(ts) - 1) / (ts[-1] - ts[0])
        grid = ts[0] + np.arange(len(ts)) / fs
        rs = lambda x: np.interp(grid, ts, x)
        us = rs(u[sel]); us -= us.mean()
        fig, ax = plt.subplots(3, 1, figsize=(10, 9), sharex=True)
        print(f"session {s}: {sel.sum() / fs:.1f} s at {fs:.0f} Hz, signal rms {us.std():.1f}")
        for k, name in enumerate(["roll", "pitch", "yaw"]):
            y = rs(gyro[k][sel]); y -= y.mean()
            f, h, coh, puu, pyy = transfer(us, y, fs, a.nperseg)
            pk = peaks(f, h, coh)
            if not pk:
                print(f"  gyro {name}: insufficient data (max coherence {np.nanmax(coh[f > 10]):.2f})")
            for p in pk:
                print(f"  gyro {name}: {p['frequency_hz']:7.1f} Hz  bw {p['bandwidth_hz']:6.1f}  Q {p['q']:5.1f}  "
                      f"gain {p['gain']:.3f} deg/s/unit  coh {p['coherence']:.2f}")
                rows.append({"session": s, "axis": "gyro_" + name, **p})
            ax[0].semilogy(f, np.abs(h), label=name)
            ax[1].plot(f, coh, label=name)
            ax[2].semilogy(f, pyy, label=name)
        # motor response: command signal -> measured rpm of the excited motor (bidirectional DShot)
        poles = int(hdr.get("motor_poles", 14) or 14)
        erpm = [col(names, d, f"eRPM[{k}]") for k in range(8)]
        erpm = [e for e in erpm if e is not None]
        if erpm:
            means = [np.nanmean(e[sel]) for e in erpm]
            k = int(np.argmax(means))
            rpm = rs(erpm[k][sel]) * 100.0 / (poles / 2)
            f, h, coh, _, _ = transfer(us, rpm - rpm.mean(), fs, a.nperseg)
            lowband = (f >= 20) & (f <= 40)
            g0 = np.nanmean(np.abs(h[lowband]))
            above = (f > 40) & (np.abs(h) < g0 / np.sqrt(2)) & (coh > 0.3)
            corner = f[above][0] if above.any() else float("nan")
            good = f[(coh >= MIN_COHERENCE) & (f >= 10)]
            print(f"  motor {k + 1}: mean {rpm.mean():.0f} rpm, gain {g0:.1f} rpm/unit at 20-40 Hz, -3 dB at {corner:.0f} Hz, "
                  f"coherent (>{MIN_COHERENCE}) up to {good.max() if good.size else float('nan'):.0f} Hz")
            rows.append({"session": s, "axis": f"rpm_motor{k + 1}", "frequency_hz": float(corner), "bandwidth_hz": float("nan"),
                         "q": float("nan"), "gain": float(g0), "coherence": float(np.nanmax(coh[lowband]))})
            ax0b = ax[0].twinx(); ax0b.semilogy(f, np.abs(h), "k", lw=0.8, label="rpm/unit"); ax0b.set_ylabel("rpm per unit")
        ax[0].set_ylabel("|H| deg/s per unit"); ax[1].set_ylabel("coherence"); ax[2].set_ylabel("gyro PSD")
        ax[1].axhline(MIN_COHERENCE, color="k", lw=0.5, ls="--")
        ax[2].set_xlabel("Hz"); ax[0].legend(); ax[0].set_title(f"session {s}")
        fig.tight_layout(); fig.savefig(f"{out}_s{s}.png", dpi=90); plt.close(fig)
    if rows:
        with open(out + "_peaks.csv", "w", newline="") as fcsv:
            w = csv.DictWriter(fcsv, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
        print("peaks ->", out + "_peaks.csv")


if __name__ == "__main__":
    main()
