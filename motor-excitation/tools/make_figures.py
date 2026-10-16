#!/usr/bin/env python3
"""Figures for the motor excitation PR."""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import bbl_report as br
import tap_report as tr
import tap_classify as tc
from repeatability import clusters
from mimo import mimo_transfer

OUT = "pr-assets/motor-excitation/"
SURF, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
CAT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]
NAME = {1: "motor 1 (rear right)", 2: "motor 2 (front right)", 3: "motor 3 (rear left)", 4: "motor 4 (front left)",
        5: "battery (from above)", 6: "frame bottom (from below)"}
plt.rcParams.update({"figure.facecolor": SURF, "axes.facecolor": SURF, "axes.edgecolor": GRID, "axes.labelcolor": INK2,
                     "xtick.color": INK2, "ytick.color": INK2, "text.color": INK, "font.size": 10,
                     "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
                     "axes.spines.right": False, "legend.frameon": False})


def fig_taps():
    names, d, hdr = br.load("logs/tap_validate1.bbl")
    col = lambda n: br.col(names, d, n)
    t = col("time") * 1e-6; o = np.argsort(t); t = t[o]
    g = np.column_stack([col(f"gyroUnfilt[{k}]")[o] * 0.1 for k in range(3)])
    acc = np.column_stack([col(f"accSmooth[{k}]")[o] for k in range(3)]) / 2048
    st = col("debug[3]")[o].astype(int) >> 4; mk = col("debug[2]")[o].astype(int)
    fs = (len(t) - 1) / (t[-1] - t[0]); rec = np.flatnonzero(st == 2); a, b = rec[0], rec[-1]
    on = [a + i for i in tr.find_taps(g[a:b], fs)[0]]
    labels, thr = tc.label_taps(t, g, acc, on, fs)
    n = int(0.004 * fs)
    fig = plt.figure(figsize=(9.6, 5.2))
    ax = fig.add_subplot(1, 2, 1, projection="polar")
    ax.set_facecolor(SURF)
    rmax = 0.42
    ax.fill_between(np.linspace(0, 2 * np.pi, 200), 0, thr, color="#efeeea", zorder=0)
    for m, (r_, p_) in tc.MIXER_QUADX.items():
        ang = np.arctan2(p_, r_)
        ax.plot([ang, ang], [thr, rmax], ls="--", lw=1, color=INK2, zorder=1)
        ax.text(ang, rmax * 1.08, f"M{m}", ha="center", va="center", color=INK2, fontsize=9)
    ok = 0; total = 0
    for i, lab in zip(on, labels):
        truth = (mk[i] >> 4) if (mk[i] & 0xF) == 3 else 0
        if not truth:
            continue
        imp = (g[i:i + n] - g[i - 20:i].mean(axis=0)).sum(axis=0) / fs
        dz = acc[i:i + int(0.02 * fs), 2] - acc[i - 40:i - 5, 2].mean()
        ang, r = np.arctan2(imp[1], imp[0]), np.hypot(imp[0], imp[1])
        marker = "o" if truth <= 4 else ("v" if tc.first_excursion(dz) < 0 else "^")
        ax.scatter([ang], [min(r, rmax)], s=52, marker=marker, color=CAT[truth - 1], edgecolor=SURF, linewidth=1.5, zorder=3)
        total += 1; ok += lab == truth
    ax.set_rmax(rmax); ax.set_rticks([0.1, 0.2, 0.3]); ax.set_yticklabels(["0.1°", "0.2°", "0.3°"], color=INK2, fontsize=8)
    ax.set_xticks(np.radians([0, 90, 180, 270])); ax.set_xticklabels(["+roll", "+pitch", "−roll", "−pitch"], color=INK2)
    ax.grid(color=GRID, linewidth=0.6)
    fig.suptitle(f"Tap auto-detect: rotational impulse in the first 4 ms; shuffled series of 5, {ok}/{total} labelled correctly",
                 fontsize=10, color=INK, x=0.02, ha="left")
    # legend panel
    lx = fig.add_subplot(1, 2, 2); lx.axis("off"); lx.set_xlim(0, 1); lx.set_ylim(0, 1)
    for k in range(1, 7):
        y = 0.92 - (k - 1) * 0.09
        mkr = "o" if k <= 4 else ("v" if k == 5 else "^")
        lx.scatter([0.04], [y], s=52, marker=mkr, color=CAT[k - 1], edgecolor=SURF, linewidth=1.5)
        lx.text(0.09, y, NAME[k], va="center", color=INK)
    steps = ["1. find taps: jump of |d gyro/dt| far above the window noise",
             "2. integrate roll/pitch rate over 4 ms → impulse vector",
             "3. split arm / centre at the widest magnitude gap",
             "    (≥ 3 taps each side; grey disc = centre zone)",
             "4. arm = mixer motor whose (roll, pitch) points closest",
             "5. centre: first vertical acc excursion < 0 → battery,",
             "    > 0 → frame bottom, |acc| < 0.03 g → tap again",
             "no calibration, no frequencies, no frame constants"]
    for j, s in enumerate(steps):
        lx.text(0.0, 0.33 - j * 0.055, s, va="center", color=INK2 if j < 7 else INK, fontsize=9)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(OUT + "tap_autodetect.png", dpi=130)
    plt.close(fig)


def fig_supports():
    stages = [("table", "logs/rep_table_report_peaks.csv"), ("in hand", "logs/stage2_hand_report_peaks.csv"),
              ("suspended", "logs/stage3_hung_report_peaks.csv")]
    data = {n: [c for c in clusters(p) if c["n"] >= 2 and c["f"] >= 30] for n, p in stages}
    ref = data["suspended"]
    score = {}
    for n, cs in data.items():
        if n == "suspended":
            continue
        hit = sum(1 for r in ref if any(c["motor"] == r["motor"] and c["axis"] == r["axis"] and abs(c["f"] - r["f"]) <= 8 for c in cs))
        score[n] = 100.0 * hit / len(ref)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 3.8), gridspec_kw={"width_ratios": [3.2, 1]})
    for row, (n, _) in enumerate(stages):
        cs = data[n]
        f = np.array([c["f"] for c in cs]); gn = np.array([c["gain"] for c in cs])
        a1.scatter(f, np.full(len(f), row), s=18 + 900 * gn, color=CAT[0], alpha=0.55, edgecolor=SURF, linewidth=1.2)
    for fm, lab, y in [(209, "209 Hz craft", 2.62), (268, "268 Hz front arms", 2.38), (445, "434-456 Hz motor", 2.62)]:
        a1.axvline(fm, color=INK2, lw=0.8, ls="--")
        a1.text(fm + 4, y, lab, color=INK2, fontsize=8, va="center")
    a1.axvspan(120, 132, color="#efeeea", zorder=0); a1.text(124, -0.55, "table\nsupport", color=INK2, fontsize=8, ha="center")
    a1.set_yticks([0, 1, 2]); a1.set_yticklabels([s[0] for s in stages]); a1.set_ylim(-0.8, 2.8)
    a1.set_xlim(30, 620); a1.set_xlabel("Hz"); a1.grid(axis="y", visible=False)
    a1.set_title("Repeatable peaks (≥ 2 of 3 runs), chirp per motor, no props; dot area = gain", fontsize=10, loc="left")
    names = list(score)
    a2.barh(range(len(names)), [score[n] for n in names], color=CAT[0], height=0.5)
    for i, n in enumerate(names):
        a2.text(score[n] + 2, i, f"{score[n]:.0f} %", va="center", color=INK)
    a2.set_yticks(range(len(names))); a2.set_yticklabels(names); a2.set_xlim(0, 100)
    a2.set_title("suspended peaks reproduced", fontsize=10, loc="left"); a2.grid(axis="y", visible=False)
    a2.set_xlabel("%")
    fig.tight_layout()
    fig.savefig(OUT + "support_comparison.png", dpi=130)
    plt.close(fig)


def fig_mimo():
    names, d, hdr = br.load("logs/full_free2.bbl")
    col = lambda n: br.col(names, d, n)
    t = col("time") * 1e-6; o = np.argsort(t); t = t[o]
    g = col("gyroUnfilt[1]")[o] * 0.1
    sess = col("debug[4]")[o].astype(int); st = col("debug[3]")[o].astype(int) >> 4
    lo, hi = [int(x) for x in str(hdr.get("motorOutput", "158,2047")).strip("[]").split(",")]
    motors = np.column_stack([col(f"motor[{k}]")[o] for k in range(4)]) * 1000.0 / (hi - lo)
    rpm = col("eRPM[0]")[o] * 100 / 7
    sel = (sess == 1002) & (st == 2)
    ts = t[sel]; fs = (len(ts) - 1) / (ts[-1] - ts[0]); grid = ts[0] + np.arange(len(ts)) / fs
    U = np.column_stack([np.interp(grid, ts, motors[sel, i]) for i in range(4)]); U -= U.mean(axis=0)
    y = np.interp(grid, ts, g[sel]); y -= y.mean()
    f, H, coh = mimo_transfer(U, y, fs, 4096)
    frot = np.mean(rpm[sel]) / 60
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(9.6, 5.6), sharex=True, gridspec_kw={"height_ratios": [3, 1]})
    good = coh >= 0.6                     # only where the inputs explain the output
    for i in range(4):
        a1.plot(f, np.where(good, np.abs(H[:, i]), np.nan), lw=2 if i in (1, 3) else 1.2, color=CAT[i], label=NAME[i + 1])
    a1.set_ylabel("pitch |H|, deg/s per command unit"); a1.set_xlim(40, 700); a1.set_ylim(0, None)
    a1.legend(loc="upper right", fontsize=9)
    for fm, lab in [(209, "209"), (268, "268"), (430, "430")]:
        a1.annotate(lab + " Hz", (fm, a1.get_ylim()[1] * 0.92), color=INK2, fontsize=8, ha="center")
    a1.axvline(frot, color=INK2, lw=0.8, ls=":")
    a1.text(frot - 3, a1.get_ylim()[1] * 0.5, f"rotation {frot:.0f} Hz", color=INK2, fontsize=8, ha="right", rotation=90)
    a1.set_title("All four motors at once, independent noise per motor, base 1300, 15 s (shown where coherence ≥ 0.6)",
                 fontsize=10, loc="left")
    a2.plot(f, coh, color=INK2, lw=1.2); a2.axhline(0.6, color=GRID, lw=1, ls="--")
    a2.set_ylim(0, 1); a2.set_ylabel("multiple\ncoherence"); a2.set_xlabel("Hz")
    fig.tight_layout()
    fig.savefig(OUT + "all_motors_mimo.png", dpi=130)
    plt.close(fig)


import sys as _s
for name in (_s.argv[1:] or ["taps", "supports", "mimo"]):
    {"taps": fig_taps, "supports": fig_supports, "mimo": fig_mimo}[name](); print(name)
