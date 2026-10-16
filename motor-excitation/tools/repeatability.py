#!/usr/bin/env python3
"""repeatability.py peaks.csv [--tol 6] : cluster peaks of repeated runs per motor/axis (session id = 7RM),
print clusters seen in >= 2 runs with frequency spread, gain CV, Q, and how many runs saw them."""
import csv, sys, collections
import numpy as np

def clusters(path, tol=6.0):
    """Peaks of repeated runs grouped per motor/axis; returns list of dicts (n = runs that saw the peak)."""
    rows = [r for r in csv.DictReader(open(path)) if r["axis"].startswith("gyro")]
    groups = collections.defaultdict(list)
    for r in rows:
        s = int(r["session"]); motor, rep = s % 10, (s // 10) % 10
        groups[(motor, r["axis"])].append((rep, float(r["frequency_hz"]), float(r["gain"]), float(r["q"]), float(r["coherence"])))

    out = []
    for (motor, axis), peaks in sorted(groups.items()):
        reps = sorted(set(p[0] for p in peaks))
        clusters = []
        for p in sorted(peaks, key=lambda p: -p[2]):          # strongest first
            for c in clusters:
                if abs(np.mean([q[1] for q in c]) - p[1]) <= tol and p[0] not in [q[0] for q in c]:
                    c.append(p); break
            else:
                clusters.append([p])
        for c in clusters:
            f = np.array([q[1] for q in c]); g = np.array([q[2] for q in c]); qq = np.array([q[3] for q in c])
            out.append({"motor": motor, "axis": axis[5:], "runs": f"{len(c)}/{len(reps)}", "n": len(c),
                        "f": f.mean(), "f_sd": f.std(ddof=1) if len(c) > 1 else np.nan,
                        "gain": g.mean(), "gain_cv": 100 * g.std(ddof=1) / g.mean() if len(c) > 1 else np.nan,
                        "q": qq.mean(), "coh": np.mean([q[4] for q in c])})

    return out


if __name__ != "__main__":
    pass
else:
  path = sys.argv[1]
  tol = float(sys.argv[sys.argv.index("--tol") + 1]) if "--tol" in sys.argv else 6.0
  out = clusters(path, tol)
  rep = [o for o in out if o["n"] >= 2]
  single = [o for o in out if o["n"] == 1]
  print(f"{'motor':>5} {'axis':>5} {'runs':>5} {'f Hz':>7} {'sd':>5} {'gain':>6} {'cv%':>5} {'Q':>5} {'coh':>5}")
  for o in sorted(rep, key=lambda o: (o["motor"], o["axis"], o["f"])):
      print(f"{o['motor']:>5} {o['axis']:>5} {o['runs']:>5} {o['f']:7.1f} {o['f_sd']:5.1f} {o['gain']:6.3f} {o['gain_cv']:5.0f} {o['q']:5.0f} {o['coh']:5.2f}")
  print(f"\npeaks seen in 2+ runs: {len(rep)}, in one run only: {len(single)}")
  # structure check: clusters present for all motors at the same frequency
  allm = collections.defaultdict(list)
  for o in rep:
      allm[(o["axis"], round(o["f"] / 10))].append(o)
  print("\nseen on every motor (same axis, within 10 Hz):")
  for (axis, _), v in sorted(allm.items()):
      if len(set(o["motor"] for o in v)) == 4:
          fs = [o["f"] for o in v]
          print(f"  {axis}: {np.mean(fs):.1f} Hz (motors {min(fs):.1f}-{max(fs):.1f}), gain {min(o['gain'] for o in v):.3f}-{max(o['gain'] for o in v):.3f}")
