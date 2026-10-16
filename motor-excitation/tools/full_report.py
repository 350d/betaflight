#!/usr/bin/env python3
"""Report for the quick full test (full_test.py): phase 1 taps (RECORD session, markers) and phase 2 all motors with
independent noise at two base speeds. Prints taps per point, per-motor transfer peaks per base, motor rotation lines,
and a mode summary with a classification.

  full_report.py logs/full1.bbl
"""
import sys
import numpy as np
from scipy import signal

import bbl_report as br
import tap_report as tr
import tap_classify as tc
from mimo import mimo_transfer

NPERSEG = 4096
MIN_COH = 0.6
AXES = ["roll", "pitch", "yaw"]


def main():
    path = sys.argv[1]
    names, d, hdr = br.load(path)
    col = lambda n: br.col(names, d, n)
    gs = 0.1 if int(hdr.get("blackbox_high_resolution", 0) or 0) else 1.0
    t = col("time") * 1e-6
    order = np.argsort(t)
    d = d[order]; t = t[order]
    g = np.column_stack([col(f"gyroUnfilt[{k}]")[order] * gs for k in range(3)])
    state = col("debug[3]")[order].astype(int) >> 4
    sess = col("debug[4]")[order].astype(int)
    mk = col("debug[2]")[order]
    lo, hi = [int(x) for x in str(hdr.get("motorOutput", "158,2047")).strip("[]").split(",")]
    motors = np.column_stack([col(f"motor[{k}]")[order] for k in range(4)]) * 1000.0 / (hi - lo)   # command units
    poles = int(hdr.get("motor_poles", 14) or 14)
    erpm = [col(f"eRPM[{k}]") for k in range(4)]
    rpm = np.column_stack([e[order] * 100.0 / (poles / 2) for e in erpm]) if erpm[0] is not None else None

    modes = []   # (f, source, detail)
    # ---- phase 1: taps ----
    rec = (state == 2) & (sess == 950)
    if rec.sum() > 1000:
        taps_by_label = None
        free = np.flatnonzero(rec & (mk.astype(int) == 7 * 16 + 2))
        if free.size > 1000:   # free window: label every tap from the gyro/acc vectors
            acc = np.column_stack([col(f"accSmooth[{k}]")[order] for k in range(3)]) / float(hdr.get("acc_1G", 2048) or 2048)
            fs0 = (len(t) - 1) / (t[-1] - t[0])
            a0, b0 = free[0], free[-1]
            onsets = [a0 + i for i in tr.find_taps(g[a0:b0], fs0)[0]]
            labels, thr = tc.label_taps(t, g, acc, onsets, fs0)
            taps_by_label = {}
            for i, lab in zip(onsets, labels):
                if lab:
                    taps_by_label.setdefault(lab, []).append(t[i])
            counts = {lab: labels.count(lab) for lab in sorted(set(labels))}
            print(f"free window: {len(onsets)} taps, labels {counts} (0 = unsure), arm/centre split {thr:.3f} deg")
        out, fs = tr.analyse(t[rec], g[rec], mk[rec], taps_by_label=taps_by_label)
        label = {1: "arm 1", 2: "arm 2", 3: "arm 3", 4: "arm 4", 5: "battery", 6: "frame bottom"}
        print(f"phase 1 taps ({fs:.0f} Hz):")
        for p, r in sorted(out.items()):
            cells = []
            for axis, ms in r["axes"].items():
                for x in ms:
                    if x["f"] >= 60 and x["amp"] >= 0.2:
                        cells.append(f"{axis[0]} {x['f']:.0f}/Q{x['q']:.0f}")
                        modes.append((x["f"], "tap " + label[p], x["q"], axis))
            print(f"  {label[p]:12s} {r['taps']} taps: " + ", ".join(cells))
    # ---- phase 2: all motors, independent noise ----
    bases = {}
    for s in sorted(set(sess[state == 2]) - {950}):
        sel = (sess == s) & (state == 2)
        if sel.sum() < 4 * NPERSEG:
            continue
        ts = t[sel]; fs = (len(ts) - 1) / (ts[-1] - ts[0]); grid = ts[0] + np.arange(len(ts)) / fs
        rs = lambda x: np.interp(grid, ts, x)
        U = np.column_stack([rs(motors[sel, i]) for i in range(4)]); U -= U.mean(axis=0)
        base = {1001: 1150, 1002: 1300}.get(int(s), int(s))   # full_test.py sessions
        mean_rpm = [float(np.mean(rpm[sel, i])) for i in range(4)] if rpm is not None else [np.nan] * 4
        frot = np.mean(mean_rpm) / 60.0
        print(f"\nphase 2 session {s}: {sel.sum() / fs:.1f} s, input rms {np.round(U.std(axis=0), 1)} units, "
              f"rpm {np.round(mean_rpm, 0)}, rotation {frot:.1f} Hz")
        found = []
        for k, axis in enumerate(AXES):
            y = rs(g[sel, k]); y -= y.mean()
            f, H, coh = mimo_transfer(U, y, fs, NPERSEG)
            for i in range(4):
                pk = br.peaks(f, H[:, i], coh, fmin=40.0, min_coh=MIN_COH, max_peaks=4)
                for p in pk:
                    found.append((p["frequency_hz"], axis, i + 1, p["gain"], p["q"], p["coherence"]))
            # rotation lines in the gyro spectrum
            fp, pyy = signal.welch(y, fs, nperseg=NPERSEG)
            idx, _ = signal.find_peaks(np.log(pyy + 1e-12), prominence=2.0)
            lines = [fp[j] for j in idx if fp[j] > 40 and any(abs(fp[j] - h * frot) < 3 for h in (1, 2, 3, 4))]
            if lines:
                print(f"  {axis}: rotation lines at {', '.join(f'{x:.0f}' for x in lines)} Hz "
                      f"(k x {frot:.0f} Hz)")
        for fr, axis, mtr, gain, q, ch in sorted(found):
            print(f"  {axis:5s} motor {mtr}: {fr:6.1f} Hz gain {gain:.3f} Q {q:.0f} coh {ch:.2f}")
            modes.append((fr, f"motors base {base:.0f}", q, axis))
        bases[s] = (base, found, frot)
        # motor response, input i -> own rpm
        if rpm is not None:
            gains = []
            for i in range(4):
                f, h, c, _, _ = br.transfer(U[:, i], rs(rpm[sel, i]) - rpm[sel, i].mean(), fs, NPERSEG // 4)
                band = (f >= 20) & (f <= 40)
                gains.append(np.nanmean(np.abs(h[band])))
            print(f"  command -> rpm at 20-40 Hz: {np.round(gains, 1)} rpm/unit")

    # ---- summary: cluster all evidence by frequency ----
    print("\nmode summary:")
    modes.sort()
    clusters = []
    for m in modes:
        if clusters and m[0] - np.mean([x[0] for x in clusters[-1]]) <= 6:
            clusters[-1].append(m)
        else:
            clusters.append([m])
    nb = len(bases)
    for c in clusters:
        f = np.mean([x[0] for x in c]); src = sorted(set(x[1] for x in c)); q = np.nanmedian([x[2] for x in c])
        taps = [s for s in src if s.startswith("tap")]; mot = [s for s in src if s.startswith("motors")]
        rotating = any(abs(f - h * b[2]) < 4 for b in bases.values() for h in (1, 2, 3, 4))
        if mot and len(mot) < nb and not taps:
            kind = "one speed only (check rotation lines)"
        elif taps and all(s in ("tap battery", "tap frame bottom") for s in taps) and not mot:
            kind = "centre" + (" / possibly grip" if q < 10 and f < 150 else "")
        elif taps and mot:
            kind = "frame (tap + motors)"
        elif mot and not taps:
            kind = "motor-driven only (motor/bell-local?)"
        else:
            kind = "frame (tap only)" + (" / possibly grip" if q < 10 and f < 150 else "")
        if rotating:
            kind += " / near a rotation harmonic"
        print(f"  {f:6.1f} Hz  Q~{q:4.0f}  {kind:45s} {', '.join(src)}")


if __name__ == "__main__":
    main()
