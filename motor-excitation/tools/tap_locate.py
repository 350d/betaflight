#!/usr/bin/env python3
"""Which arm was tapped, from the gyro alone: the first milliseconds after a tap give a rotation impulse whose
roll/pitch direction points at the arm. Prints the impulse angle per tap, grouped by the arm that was prompted."""
import sys
import numpy as np
import bbl_report as br
import tap_report as tr

IMPULSE_S = 0.004


def taps_with_impulse(path):
    names, d, hdr = br.load(path)
    col = lambda n: br.col(names, d, n)
    gs = 0.1 if int(hdr.get("blackbox_high_resolution", 0) or 0) else 1.0
    t = col("time") * 1e-6; o = np.argsort(t); t = t[o]
    g = np.column_stack([col(f"gyroUnfilt[{k}]")[o] * gs for k in range(3)])
    st = col("debug[3]")[o].astype(int) >> 4; mk = col("debug[2]")[o].astype(int)
    fs = (len(t) - 1) / (t[-1] - t[0])
    out = []
    for arm in range(1, 7):
        sel = np.flatnonzero((st == 2) & (mk == arm * 16 + 2))
        if sel.size < 1000:
            continue
        a, b = sel[0], sel[-1]
        taps, _ = tr.find_taps(g[a:b], fs)
        n = int(IMPULSE_S * fs)
        for i in taps:
            i += a
            pre = g[i - 20:i].mean(axis=0)
            imp = (g[i:i + n] - pre).sum(axis=0) / fs        # integrated rate over the first ms: angle, deg
            ang = np.degrees(np.arctan2(imp[1], imp[0]))     # direction in the roll/pitch plane
            out.append((arm, imp, ang))
    return out


for path in sys.argv[1:]:
    print(path)
    res = taps_with_impulse(path)
    for arm in sorted(set(r[0] for r in res)):
        angs = [r[2] for r in res if r[0] == arm]
        mags = [np.hypot(r[1][0], r[1][1]) for r in res if r[0] == arm]
        print(f"  point {arm}: angles {', '.join(f'{x:+4.0f}' for x in angs)}  (impulse {np.median(mags):.3f} deg)")
