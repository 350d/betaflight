#!/usr/bin/env python3
"""compare_stages.py NAME=peaks.csv NAME=peaks.csv ... : per motor/axis, match repeatable peaks (seen in >= 2 runs)
between mounting stages. A mode that keeps its frequency on every support belongs to the craft; one that moves
or appears/disappears depends on the support."""
import sys
import numpy as np
from repeatability import clusters

TOL = 8.0
stages = [a.split("=", 1) for a in sys.argv[1:]]
data = {name: [c for c in clusters(path) if c["n"] >= 2 and c["f"] >= 30] for name, path in stages}
names = [n for n, _ in stages]

# chain modes across stages: start from every peak, attach the nearest peak of each other stage on the same motor/axis
rows = []
used = {n: set() for n in names}
for n0 in names:
    for i, c in enumerate(data[n0]):
        if i in used[n0]:
            continue
        used[n0].add(i)
        row = {n0: c}
        for n1 in names:
            if n1 == n0:
                continue
            best = None
            for j, d in enumerate(data[n1]):
                if j in used[n1] or d["motor"] != c["motor"] or d["axis"] != c["axis"]:
                    continue
                if abs(d["f"] - c["f"]) <= TOL and (best is None or abs(d["f"] - c["f"]) < abs(data[n1][best]["f"] - c["f"])):
                    best = j
            if best is not None:
                used[n1].add(best)
                row[n1] = data[n1][best]
        rows.append((c["motor"], c["axis"], row))

hdr = f"{'motor':>5} {'axis':>5} " + " ".join(f"{n:>18}" for n in names)
print(hdr + "   (f Hz / gain deg/s/unit)")
for motor, axis, row in sorted(rows, key=lambda r: (r[0], r[1], min(c["f"] for c in r[2].values()))):
    cells = [f"{row[n]['f']:7.1f} / {row[n]['gain']:.3f}" if n in row else f"{'-':>18}" for n in names]
    print(f"{motor:>5} {axis:>5} " + " ".join(f"{c:>18}" for c in cells))

print("\nmodes present on every stage for all four motors (any axis):")
for motor, axis, row in rows:
    pass
common = {}
for motor, axis, row in rows:
    if len(row) == len(names):
        key = round(np.mean([c["f"] for c in row.values()]) / 15)
        common.setdefault(key, set()).add(motor)
for key, motors in sorted(common.items()):
    fs = [np.mean([c["f"] for c in row.values()]) for m, a, row in rows if len(row) == len(names) and round(np.mean([c["f"] for c in row.values()]) / 15) == key]
    print(f"  ~{np.mean(fs):.0f} Hz: motors {sorted(motors)}")
