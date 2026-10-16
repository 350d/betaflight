#!/usr/bin/env python3
"""Calibration-free tap classifier: which arm or centre point was tapped, from gyro + acc only.
Vectors only, no frequencies and no frame constants:
- arm vs centre: the rotational impulse (roll/pitch rate integrated over 4 ms) of the session's taps splits into two
  groups (arms large, centre small); the threshold is the widest gap in log magnitude (fallback ARM_MIN_DEG);
- arm: the mixer motor whose (roll, pitch) coefficients point closest to the impulse direction (taps from above);
- centre: sign of the vertical acc impulse: < 0 = tapped from above (battery on top), > 0 = from below (frame bottom);
  |acc z| < UNSURE_G -> 0 = "not sure, tap again".
  tap_classify.py logs/*.bbl     prints confusion against the prompted point
"""
import sys
import numpy as np
import bbl_report as br
import tap_report as tr

# Betaflight quad-X mixer (roll, pitch) per motor 1..4
MIXER_QUADX = {1: (-1.0, 1.0), 2: (-1.0, -1.0), 3: (1.0, 1.0), 4: (1.0, -1.0)}
ARM_MIN_DEG = 0.08      # fallback only, when the session has no clear two-group split
UNSURE_G = 0.03
IMPULSE_S = 0.004


def split_threshold(mags, min_group=3, min_ratio=1.4):
    """Widest gap in log(magnitude) with at least min_group taps on each side (a single weak tap must not set the
    split); None if no gap reaches min_ratio."""
    x = np.sort(np.log(np.asarray(mags) + 1e-9))
    best, j_best = 0.0, None
    for j in range(min_group - 1, len(x) - min_group):
        gap = x[j + 1] - x[j]
        if gap > best:
            best, j_best = gap, j
    if j_best is None or best < np.log(min_ratio):
        return None
    return float(np.exp((x[j_best] + x[j_best + 1]) / 2))


def first_excursion(dz, frac=0.5):
    """Sign-carrying value of the first excursion beyond frac * max |dz| (the force direction; later lobes of the
    smoothed acc ring the other way)."""
    a = np.abs(dz)
    if a.max() <= 0:
        return 0.0
    i = int(np.argmax(a >= frac * a.max()))
    return float(dz[i])


def classify(imp_roll, imp_pitch, acc_z, threshold=ARM_MIN_DEG, mixer=MIXER_QUADX):
    mag = np.hypot(imp_roll, imp_pitch)
    if mag >= threshold:
        ang = np.arctan2(imp_pitch, imp_roll)
        return min(mixer, key=lambda m: abs(np.angle(np.exp(1j * (ang - np.arctan2(mixer[m][1], mixer[m][0]))))))
    if abs(acc_z) < UNSURE_G:
        return 0
    return 5 if acc_z < 0 else 6


def label_taps(t, g, acc, onsets, fs):
    """onsets: indices into t. Returns labels (0 = unsure, 1-4 arms, 5 battery, 6 frame bottom) and the split."""
    n = int(IMPULSE_S * fs)
    feats = []
    for i in onsets:
        imp = (g[i:i + n] - g[i - 20:i].mean(axis=0)).sum(axis=0) / fs
        dz = acc[i:i + int(0.02 * fs), 2] - acc[i - 40:i - 5, 2].mean()
        feats.append((imp[0], imp[1], first_excursion(dz)))
    thr = split_threshold([np.hypot(f[0], f[1]) for f in feats]) or ARM_MIN_DEG
    return [classify(f[0], f[1], f[2], thr) for f in feats], thr


def taps(path):
    names, d, hdr = br.load(path)
    col = lambda n: br.col(names, d, n)
    gs = 0.1 if int(hdr.get("blackbox_high_resolution", 0) or 0) else 1.0
    t = col("time") * 1e-6; o = np.argsort(t); t = t[o]
    g = np.column_stack([col(f"gyroUnfilt[{k}]")[o] * gs for k in range(3)])
    acc = np.column_stack([col(f"accSmooth[{k}]")[o] for k in range(3)]) / float(hdr.get("acc_1G", 2048) or 2048)
    st = col("debug[3]")[o].astype(int) >> 4; mk = col("debug[2]")[o].astype(int)
    fs = (len(t) - 1) / (t[-1] - t[0])
    feats = []
    for point in range(1, 7):
        sel = np.flatnonzero((st == 2) & (mk == point * 16 + 2))
        if sel.size < 1000:
            continue
        a, b = sel[0], sel[-1]
        found, _ = tr.find_taps(g[a:b], fs)
        n = int(IMPULSE_S * fs)
        for i in found:
            i += a
            imp = (g[i:i + n] - g[i - 20:i].mean(axis=0)).sum(axis=0) / fs
            dz = acc[i:i + int(0.02 * fs), 2] - acc[i - 40:i - 5, 2].mean()
            feats.append((point, imp[0], imp[1], first_excursion(dz)))
    thr = split_threshold([np.hypot(f[1], f[2]) for f in feats]) or ARM_MIN_DEG
    return [(f[0], classify(f[1], f[2], f[3], thr)) for f in feats], thr


if __name__ == "__main__":
    allrows = []
    for p in sys.argv[1:]:
        r, thr = taps(p)
        ok = sum(1 for a, b in r if a == b)
        unsure = sum(1 for a, b in r if b == 0)
        print(f"{p}: {ok}/{len(r)} correct, {unsure} unsure, arm/centre split {thr:.3f} deg"
              + "".join(f"  [prompted {a} -> {b}]" for a, b in r if a != b))
        allrows += r
    ok = sum(1 for a, b in allrows if a == b)
    unsure = sum(1 for a, b in allrows if b == 0)
    wrong = len(allrows) - ok - unsure
    print(f"total: {ok}/{len(allrows)} correct ({100 * ok / max(1, len(allrows)):.0f} %), {unsure} unsure, {wrong} wrong")
