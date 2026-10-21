#!/usr/bin/env python3
"""The D-term search's delay / noise front on three 5" hovers, from the firmware's own decision run on the host
(fthost/host_dt3: filter_autotune.c with stubs), with the three presets marked.

  make_front_plot.py OUT.png PREFIX:LABEL ...   (PREFIX_m<budget>.bin manual sweep, PREFIX_p<preset>.bin presets)
"""
import glob
import re
import struct
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def point(path):
    """D x10, P+D delay ms, stock delay ms, added ripple heat (host model units) of the decision in a status dump."""
    p = open(path, "rb").read()
    n = len(p)
    delay, delay_ref = struct.unpack_from("<HH", p, n - 53)
    heat_ref, heat = struct.unpack_from("<HH", p, n - 31)
    return p[n - 60] / 10, delay / 1000, delay_ref / 1000, (heat - heat_ref) / 100


def front(points):
    """The least delay for each heat: sorted by heat, only points that lower the delay."""
    out = []
    for h, d in sorted(points):
        if not out or d < out[-1][1] - 1e-6:
            out.append((h, d))
    return out


def main():
    out = sys.argv[1]
    plt.style.use("dark_background")
    fig, ax = plt.subplots(figsize=(8.5, 5))
    colours = ["#7fb3ff", "#ffb347", "#9be37f"]
    marks = [("Low noise", "s"), ("Balanced", "o"), ("Low delay", "^")]
    scale = None
    for (spec, col) in zip(sys.argv[2:], colours):
        prefix, label = spec.split(":", 1)
        files = glob.glob(prefix + "_m*.bin")
        pts = [point(f) for f in files]
        ref = pts[0][2]
        if scale is None:                       # added heat relative to the first hover's Balanced choice
            scale = point(f"{prefix}_p1.bin")[3]
        fr = front([(max(h, 0.0) / scale, d) for _, d, _, h in pts] + [(0.0, ref)])
        xs, ys = zip(*fr)
        ax.plot(xs, ys, "-", color=col, lw=1.6, alpha=0.9, label=f"{label} (stock {ref:.2f} ms)")
        for k, (name, m) in enumerate(marks):
            mult, delay, _, heat = point(f"{prefix}_p{k}.bin")
            ax.plot([max(heat, 0.0) / scale], [delay], m, color=col, ms=9, mec="white", zorder=3)
            if col == colours[0]:
                ax.annotate(f"{name} (D x{mult:.1f})", (max(heat, 0.0) / scale, delay), textcoords="offset points",
                            xytext=(8, 6), fontsize=9)
    ax.set_xlabel("added motor heating from the ripple, relative (Balanced of hover 1 = 1)")
    ax.set_ylabel("P+D delay at the measured crossover (ms)")
    ax.set_title("D-term search: the least delay for each added heat, three hovers", fontsize=10)
    ax.grid(alpha=0.2)
    ax.legend(fontsize=9)
    fig.savefig(out, dpi=110, bbox_inches="tight")


if __name__ == "__main__":
    main()
