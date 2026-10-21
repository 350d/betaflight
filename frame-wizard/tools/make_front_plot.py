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
    p = open(path, "rb").read()
    n = len(p)
    delay, delay_ref = struct.unpack_from("<HH", p, n - 53)
    noise = p[n - 48]
    mult = p[n - 60]
    return noise, delay / 1000, delay_ref / 1000, mult / 10


def main():
    out = sys.argv[1]
    plt.style.use("dark_background")
    fig, ax = plt.subplots(figsize=(8.5, 5))
    colours = ["#7fb3ff", "#ffb347", "#9be37f"]
    marks = [("Low noise", "s"), ("Balanced", "o"), ("Low delay", "^")]
    for (spec, col) in zip(sys.argv[2:], colours):
        prefix, label = spec.split(":", 1)
        pts = sorted({point(f)[:2] for f in glob.glob(prefix + "_m*.bin")})
        ref = point(glob.glob(prefix + "_m*.bin")[0])[2]
        xs = [100] + [x for x, _ in pts]
        ys = [ref] + [y for _, y in pts]
        ax.plot(xs, ys, "-", color=col, lw=1.2, alpha=0.8, label=f"{label} (stock {ref:.2f} ms)")
        ax.plot([100], [ref], "x", color=col, ms=8)
        for k, (name, m) in enumerate(marks):
            noise, delay, _, mult = point(f"{prefix}_p{k}.bin")
            ax.plot([noise], [delay], m, color=col, ms=9, mec="white")
            if col == colours[0]:
                ax.annotate(f"{name} (D x{mult:.1f})", (noise, delay), textcoords="offset points", xytext=(8, 6), fontsize=9)
    ax.set_xlabel("motor noise index vs the stock filters (%)")
    ax.set_ylabel("P+D delay at the measured crossover (ms)")
    ax.set_title("D-term search: the least delay for each noise, three hovers; x = stock filters", fontsize=10)
    ax.grid(alpha=0.2)
    ax.legend(fontsize=9)
    fig.savefig(out, dpi=110, bbox_inches="tight")


if __name__ == "__main__":
    main()
