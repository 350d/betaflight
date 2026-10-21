#!/usr/bin/env python3
"""Two real hovers of the 5": stock filters vs the filters the autotune applied (Balanced, 0.5 K).
Figure 1: spectrograms (frequency vs time, Blackbox Explorer colours) of the raw gyro, the filtered gyro and the
D-term, roll + pitch, both flights on one colour scale. Figure 2: the mean spectra over the steady hover.

  make_hover_compare.py STOCK.bbl TUNED.bbl T0 T1 OUTDIR
"""
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from orangebox import Parser
from scipy import signal


def load(path):
    q = Parser.load(path, 1)
    names, rows = q.field_names, []
    try:
        for fr in q.frames():
            if len(fr.data) == len(names):
                rows.append([float(x) if x not in ("", None) else np.nan for x in fr.data])
    except (IndexError, ValueError, RuntimeError):
        pass
    d = np.array(rows, dtype=float)
    c = lambda n: d[:, names.index(n)]
    t = (c("time") - c("time")[0]) * 1e-6
    fs = 1 / np.median(np.diff(t))
    sig = {
        "raw gyro": np.array([c(f"gyroUnfilt[{k}]") for k in range(2)]) * 0.1,
        "filtered gyro": np.array([c(f"gyroADC[{k}]") for k in range(2)]) * 0.1,
        "D-term": np.array([c(f"axisD[{k}]") for k in range(2)]),
    }
    return t, fs, sig, q.headers


def spec(x, fs, nfft=512, step=256):
    f, tt, s = signal.spectrogram(x - x.mean(axis=1, keepdims=True), fs=fs, nperseg=nfft, noverlap=nfft - step, axis=1)
    return f, tt, 10 * np.log10(s.sum(axis=0) + 1e-12)


def main():
    stock, tuned, t0, t1, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    flights = [("stock filters", *load(stock)), ("autotune, Balanced (0.5 K)", *load(tuned))]
    kinds = ["raw gyro", "filtered gyro", "D-term"]
    plt.style.use("dark_background")
    fig, axs = plt.subplots(2, 3, figsize=(15, 7.5), sharey=True)
    for col, kind in enumerate(kinds):
        specs = []
        for name, t, fs, sig, _ in flights:
            m = (t >= t0) & (t <= t1)
            specs.append(spec(sig[kind][:, m], fs))
        vmax = max(s[2].max() for s in specs)
        for row, (name, *_rest) in enumerate(flights):
            f, tt, s = specs[row]
            ax = axs[row, col]
            im = ax.pcolormesh(tt + t0, f, s, shading="auto", cmap="inferno", vmin=vmax - 60, vmax=vmax)
            ax.set_ylim(0, 980)
            ax.set_title(f"{kind}: {name}", fontsize=10)
            if col == 0:
                ax.set_ylabel("Hz")
            if row == 1:
                ax.set_xlabel("s")
        fig.colorbar(im, ax=axs[:, col], shrink=0.8, label="dB")
    fig.suptitle('5" hover, roll + pitch: the same scale per column', fontsize=12)
    fig.savefig(f"{out}/hover_spectrogram.png", dpi=110, bbox_inches="tight")

    fig, axs = plt.subplots(1, 2, figsize=(13, 4.2))
    colours = ["#7fb3ff", "#ffb347"]
    for k, kind in enumerate(["filtered gyro", "D-term"]):
        ax = axs[k]
        for (name, t, fs, sig, _), colr in zip(flights, colours):
            m = (t >= t0) & (t <= t1)
            x = sig[kind][:, m]
            f, p = signal.welch(x - x.mean(axis=1, keepdims=True), fs=fs, nperseg=2048, axis=1)
            band = (f > 20)
            rms = np.sqrt(p[:, band].sum() * (f[1] - f[0]) / 2)
            ax.semilogy(f, p.mean(axis=0), color=colr, lw=1.2, label=f"{name}: rms > 20 Hz {rms:.2f}")
        ax.set_xlim(0, 980)
        ax.set_xlabel("Hz")
        ax.set_title(f"{kind}, mean spectrum ({t0:.0f}-{t1:.0f} s)", fontsize=10)
        ax.legend(fontsize=9)
        ax.grid(alpha=0.2)
    fig.savefig(f"{out}/hover_spectra.png", dpi=110, bbox_inches="tight")


if __name__ == "__main__":
    main()
