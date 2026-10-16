#!/usr/bin/env python3
"""Tap test analysis: find taps in each arm window (marker arm*16+2), average the free-decay spectra per arm and
axis, pick modes, estimate damping from the decay envelope (Q = 1 / (2 zeta)). Input force is unknown, so only
frequencies, Q and relative amplitudes are meaningful.

  tap_report.py logs/tap.bbl [--compare logs/stage3_hung_report_peaks.csv]
  tap_report.py --selftest
"""
import argparse
import sys

import numpy as np
from scipy import signal

SEG_S = 0.25          # decay window after a tap
SKIP_S = 0.002        # skip the contact itself
MIN_GAP_S = 0.3
FMIN, FMAX = 30.0, 1800.0
SAT_DPS = 1990.0


def find_taps(x, fs):
    """x: (n, 3) gyro deg/s. Onsets where the summed |derivative| jumps far above the window's noise."""
    d = np.abs(np.diff(x, axis=0)).sum(axis=1)
    d = np.convolve(d, np.ones(4) / 4, mode="same")
    med = np.median(d); mad = np.median(np.abs(d - med)) + 1e-9
    thr = med + 25 * mad
    idx = np.flatnonzero(d > thr)
    taps = []
    for i in idx:
        if not taps or i - taps[-1] > MIN_GAP_S * fs:
            taps.append(i)
    return [t for t in taps if t + int(SEG_S * fs) < len(x)], thr


def decay_spectrum(seg, fs, nfft):
    w = signal.windows.tukey(len(seg), 0.1)
    return np.abs(np.fft.rfft((seg - seg.mean()) * w, nfft))


def damping(seg, fs, f0):
    """Q from the envelope of the band around f0: fit log envelope over its decay."""
    lo, hi = f0 * 0.85, min(f0 * 1.15, 0.45 * fs)
    sos = signal.butter(4, [lo, hi], btype="band", fs=fs, output="sos")
    y = signal.sosfiltfilt(sos, seg)
    env = np.abs(signal.hilbert(y))
    i0 = int(np.argmax(env[: int(0.03 * fs)]))
    e = env[i0:]
    floor = np.median(env[-int(0.05 * fs):])
    stop = np.flatnonzero(e < max(floor * 3, e[0] * 0.05))
    n = stop[0] if stop.size else len(e)
    if n < int(0.01 * fs):
        return np.nan
    t = np.arange(n) / fs
    slope = np.polyfit(t, np.log(e[:n] + 1e-12), 1)[0]
    sigma = -slope
    if sigma <= 0:
        return np.nan
    zeta = sigma / (2 * np.pi * f0)
    return 1.0 / (2 * zeta)


def analyse(t, g, markers, fs_hint=None, nfft=None, taps_by_label=None):
    """t seconds, g (n,3) deg/s, markers (n,) ints. Returns {arm: {...}}.
    taps_by_label: {label: [tap times, s]} to use instead of the marker windows (free mode)."""
    fs = (len(t) - 1) / (t[-1] - t[0])
    grid = t[0] + np.arange(len(t)) / fs
    g = np.column_stack([np.interp(grid, t, g[:, k]) for k in range(3)])
    mk = np.round(np.interp(grid, t, markers)).astype(int)
    seg_n = int(SEG_S * fs); skip = int(SKIP_S * fs)
    nfft = nfft or int(2 ** np.ceil(np.log2(fs)))      # ~1 Hz bins after zero padding
    freqs = np.fft.rfftfreq(nfft, 1 / fs)
    out = {}
    labels = sorted(taps_by_label) if taps_by_label else range(1, 9)
    for arm in labels:
        if taps_by_label:
            taps = [int(np.searchsorted(grid, x)) for x in taps_by_label[arm]]
            taps = [i for i in taps if 0 < i < len(grid) - seg_n]
        else:
            sel = np.flatnonzero(mk == arm * 16 + 2)
            if sel.size < seg_n:
                continue
            a, b = sel[0], sel[-1]
            taps, thr = find_taps(g[a:b], fs)
            taps = [a + i for i in taps]
        res = {"taps": len(taps), "saturated": 0, "axes": {}}
        segs = []
        for i in taps:
            s = g[i + skip: i + skip + seg_n]
            if np.abs(g[i: i + seg_n]).max() >= SAT_DPS:
                res["saturated"] += 1
            segs.append(s)
        if not segs:
            out[arm] = res
            continue
        for k, axis in enumerate(["roll", "pitch", "yaw"]):
            spec = np.mean([decay_spectrum(s[:, k], fs, nfft) ** 2 for s in segs], axis=0) ** 0.5
            band = (freqs >= FMIN) & (freqs <= min(FMAX, 0.45 * fs))
            m = np.where(band, spec, 0)
            pk, _ = signal.find_peaks(m, prominence=0.15 * m.max(), distance=int(8 / (freqs[1] - freqs[0])))
            pk = sorted(pk, key=lambda j: -m[j])[:6]
            modes = []
            for j in sorted(pk, key=lambda j: freqs[j]):
                qs = [damping(s[:, k], fs, freqs[j]) for s in segs]
                modes.append({"f": float(freqs[j]), "amp": float(m[j] / m.max()), "q": float(np.nanmedian(qs)),
                              "q_sd": float(np.nanstd(qs))})
            res["axes"][axis] = modes
        out[arm] = res
    return out, fs


def selftest():
    rng = np.random.default_rng(3)
    fs = 4000.0; T = 10.0; n = int(fs * T)
    t = np.arange(n) / fs
    modes = [(209.0, 40.0), (268.0, 25.0), (434.0, 60.0)]
    g = rng.standard_normal((n, 3)) * 0.3
    mk = np.zeros(n, int); mk[int(1 * fs):int(9 * fs)] = 2 * 16 + 2
    for tap in np.arange(1.5, 8.5, 1.4):
        i = int(tap * fs); tt = t[: int(0.4 * fs)]
        amp = rng.uniform(50, 150)
        for f0, q in modes:
            zeta = 1 / (2 * q)
            resp = amp * np.exp(-zeta * 2 * np.pi * f0 * tt) * np.sin(2 * np.pi * f0 * tt)
            g[i: i + len(tt), 1] += resp
            g[i: i + len(tt), 0] += 0.3 * resp
    t_jit = t + rng.normal(0, 2e-6, n)   # tiny timestamp jitter
    out, fsx = analyse(np.sort(t_jit), g, mk)
    r = out[2]
    found = r["axes"]["pitch"]
    print("taps", r["taps"], "pitch modes", [(round(x["f"], 1), round(x["q"], 1)) for x in found])
    ok = r["taps"] == 5
    for f0, q in modes:
        hit = [x for x in found if abs(x["f"] - f0) < 3]
        ok &= bool(hit) and abs(hit[0]["q"] - q) / q < 0.3
    print("SELFTEST", "PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bbl", nargs="?")
    ap.add_argument("--compare", default=None, help="chirp peaks csv of the reference stage")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    import bbl_report as br
    names, d, hdr = br.load(a.bbl)
    gs = 0.1 if int(hdr.get("blackbox_high_resolution", 0) or 0) else 1.0
    t = br.col(names, d, "time") * 1e-6
    g = np.column_stack([br.col(names, d, f"gyroUnfilt[{k}]") * gs for k in range(3)])
    sess = br.col(names, d, "debug[4]").astype(int)
    state = br.col(names, d, "debug[3]").astype(int) >> 4
    mk = br.col(names, d, "debug[2]")
    sel = state == 2
    order = np.argsort(t[sel])
    out, fs = analyse(t[sel][order], g[sel][order], mk[sel][order])
    print(f"{a.bbl}: {sel.sum()} frames in RECORD, {fs:.0f} Hz, sessions {sorted(set(sess[sel]))}")
    ref = None
    if a.compare:
        from repeatability import clusters
        ref = [c for c in clusters(a.compare) if c["n"] >= 2 and c["f"] >= 30]
    label = {5: "battery", 6: "frame bottom"}
    for arm, r in sorted(out.items()):
        print(f"{label.get(arm, 'motor')} {arm}: {r['taps']} taps, {r['saturated']} saturated")
        for axis, modes in r["axes"].items():
            cells = []
            for x in modes:
                c = ""
                if ref and arm <= 4:
                    near = [c_ for c_ in ref if c_["motor"] == arm and c_["axis"] == axis and abs(c_["f"] - x["f"]) <= 8]
                    c = f" [chirp {near[0]['f']:.0f} Q{near[0]['q']:.0f}]" if near else " [-]"
                cells.append(f"{x['f']:.0f} Hz Q{x['q']:.0f}±{x['q_sd']:.0f} a{x['amp']:.2f}{c}")
            print(f"  {axis:5s}: " + "; ".join(cells))


if __name__ == "__main__":
    main()
