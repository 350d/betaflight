#!/usr/bin/env python3
"""Blackbox Explorer style noise spectrograms (frequency vs throttle) for the PR: raw gyro, BF default filters,
the autotune's filters. Flight 5 of the 5" (hover plus manoeuvres), the raw gyro replayed through both chains.

  make_spectrogram.py LOG.bbl LOGINDEX TMIN TMAX OUT.png
"""
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from orangebox import Parser
from scipy import signal

sys.path.insert(0, "/Volumes/2TB/GITHUB/betaflight/research/motor-thermal")
import filter_sim as S  # noqa: E402


def load(path, li):
    q = Parser.load(path, li)
    names, rows = q.field_names, []
    try:
        for fr in q.frames():
            if len(fr.data) == len(names):
                rows.append([float(x) if x not in ("", None) else np.nan for x in fr.data])
    except (IndexError, ValueError, RuntimeError):
        pass
    return names, np.array(rows, dtype=float)


def spectrogram(x, thr, fs, nfft=256, step=128, bins=50):
    """Mean PSD (dB) per throttle bin, like the Blackbox Explorer 'Freq. vs Throttle' view."""
    w = np.hanning(nfft)
    acc = np.zeros((bins, nfft // 2 + 1))
    cnt = np.zeros(bins)
    for s in range(0, len(x) - nfft, step):
        seg = x[s:s + nfft]
        p = np.abs(np.fft.rfft((seg - seg.mean()) * w)) ** 2 / (fs * (w * w).sum())
        b = int(np.clip(np.mean(thr[s:s + nfft]) * bins, 0, bins - 1))
        acc[b] += p
        cnt[b] += 1
    with np.errstate(divide="ignore", invalid="ignore"):
        db = 10 * np.log10(acc / cnt[:, None])
    db[cnt == 0] = np.nan
    return np.fft.rfftfreq(nfft, 1 / fs), db, cnt


def main():
    path, li, tmin, tmax, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    names, d = load(path, li)
    c = lambda n: d[:, names.index(n)]
    t = c("time") * 1e-6
    t -= t[0]
    fs = 1 / np.median(np.diff(t))
    m = (t >= tmin) & (t <= tmax)
    gu = np.array([c(f"gyroUnfilt[{k}]")[m] * 0.1 for k in range(3)])
    rot = np.array([c(f"eRPM[{k}]")[m] * 100 / 7 / 60 for k in range(4)])
    # throttle as the mixer sees it: mean motor output 0..1 (rcCommand throttle stays near hover in angle mode)
    mot = np.array([c(f"motor[{k}]")[m] for k in range(4)]).mean(0)
    thr = np.clip((mot - 0.0) / 2047.0, 0, 1)
    rc_thr = c("rcCommand[3]")[m] / 10000.0

    default = S.Chain("defaults", rpm_w=(1, 1, 1), rpm_q=5.0, dyn_count=3, dyn_q=3.0, glpf1=(250, 500), glpf2=500)
    tuned = S.Chain("autotune", rpm_w=(0.95, 0.80, 0.90), rpm_q=3.7, dyn_count=1, dyn_q=3.0, static=((383, 360),),
                    glpf1=None, glpf2=None)
    rows = [("Raw gyro (gyroUnfilt)", gu)]
    for ch, lab in ((default, "BF default gyro filters"), (tuned, "Filters set by the autotune")):
        x, _ = S.run(ch, gu, rot, rc_thr, fs)
        rows.append((lab, x))

    vmin, vmax = -70, 10
    fig, axes = plt.subplots(3, 3, figsize=(15, 11), sharex=True, sharey=True, facecolor="black")
    for r, (lab, x) in enumerate(rows):
        for a, ax_name in enumerate(("roll", "pitch", "yaw")):
            ax = axes[r, a]
            ax.set_facecolor("black")
            F, db, cnt = spectrogram(x[a], thr, fs)
            im = ax.imshow(db, origin="lower", aspect="auto", cmap="inferno", vmin=vmin, vmax=vmax,
                           extent=[F[0], F[-1], 0, 100], interpolation="bilinear")
            ax.set_xlim(0, 900)
            used = np.where(cnt > 0)[0]
            ax.set_ylim(max(0, used.min() * 2 - 4), min(100, used.max() * 2 + 6))
            ax.set_title(f"{lab} — gyro {ax_name}", color="white", fontsize=10)
            ax.tick_params(colors="#cccccc", labelsize=8)
            for sp in ax.spines.values():
                sp.set_color("#555555")
            ax.grid(False)
            if r == 2:
                ax.set_xlabel("Hz", color="#cccccc")
            if a == 0:
                ax.set_ylabel("throttle (mean motor output, %)", color="#cccccc")
    cb = fig.colorbar(im, ax=axes, fraction=0.02, pad=0.01)
    cb.set_label("PSD (dB re 1 (deg/s)$^2$/Hz)", color="#cccccc")
    cb.ax.tick_params(colors="#cccccc")
    fig.suptitle('5" flight 5: hover and manoeuvres, the same raw gyro through both chains (Blackbox Explorer "Freq. vs Throttle" style)',
                 color="white", fontsize=12)
    fig.savefig(out, facecolor="black", dpi=110, bbox_inches="tight")
    print("saved", out, "throttle range %.0f-%.0f %%" % (thr.min() * 100, thr.max() * 100))


if __name__ == "__main__":
    main()
