"""Model vs datasheet temperature rise, before and after the prop-wash term (220 points)."""
import sys, os, types, numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S); out = sys.argv[2]; sys.argv = [sys.argv[0], sys.argv[1]]
_src = open(os.path.join(S, "dseval.py")).read(); _src = _src[:_src.index('if os.environ.get("DSEVAL_LIB")')]
dseval = types.ModuleType("dseval"); dseval.__file__ = os.path.join(S, "dseval.py"); exec(compile(_src, "dseval.py", "exec"), dseval.__dict__)
P = [p for p in dseval.P if dseval.run(p, 1.0, 0.8)[1] < 40 and p["T"] > (p["amb"] if p["amb"] is not None else 25) + 3]
brands = sorted(set(p["brand"] for p in P), key=lambda b: -sum(1 for p in P if p["brand"] == b))
cols = dict(zip(brands, ["#2a6fdb", "#e07b00", "#2e9e5b", "#c0392b", "#8e44ad", "#16a085", "#7f8c8d", "#d35400", "#34495e"]))
fig, ax = plt.subplots(1, 2, figsize=(11, 5), sharex=True, sharey=True)
for a, (vk, n, title) in zip(ax, ((0, 1, "before: no prop wash"), (1.0, 0.8, "now: prop wash, h ∝ v^0.8"))):
    for b in brands:
        xs, ys = [], []
        for p in [p for p in P if p["brand"] == b]:
            m, w, pl, pir, amb = dseval.run(p, vk, n)
            xs.append(p["T"] - amb); ys.append(max(m - amb, 1))
        a.scatter(xs, ys, s=14, alpha=0.75, color=cols[b], label="%s (%d)" % (b, len(xs)))
    lim = [3, 700]; a.plot(lim, lim, "k-", lw=1); a.plot(lim, [2 * x for x in lim], "k:", lw=0.8); a.plot(lim, [x / 2 for x in lim], "k:", lw=0.8)
    a.set_xscale("log"); a.set_yscale("log"); a.set_xlim(3, 200); a.set_ylim(1, 700); a.grid(alpha=0.3, which="both")
    a.set_title(title); a.set_xlabel("datasheet temperature rise over ambient, K")
ax[0].set_ylabel("model winding (or surface) rise, K"); ax[1].legend(fontsize=7, loc="upper left")
fig.suptitle("Full-throttle runs with props from 9 motor datasheets: model vs published (dotted: factor 2)")
fig.tight_layout(); fig.savefig(out, dpi=110)
print("saved", out)
