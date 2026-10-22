import sys, os, types, numpy as np
from scipy.stats import spearmanr
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
DS = sys.argv[1]; sys.argv = [sys.argv[0], DS]
_src = open(os.path.join(S, "dseval.py")).read(); _src = _src[:_src.index('if os.environ.get("DSEVAL_LIB")')]
dseval = types.ModuleType("dseval"); dseval.__file__ = os.path.join(S, "dseval.py"); exec(compile(_src, "dseval.py", "exec"), dseval.__dict__)
amb = lambda p: p["amb"] if p["amb"] is not None else 25.0
law = lambda p: 70 * (2800 / p["kv"]) ** 2 / ((p["h"] / 3.5) ** 1.29 * (p["d"] / 22) ** 3.95)
sel = [dict(p, r_mohm=law(p)) for p in dseval.P if p["brand"] == "BrotherHobby" and p["kind"] == "windings" and dseval.run(p, 1.0, 0.8)[1] < 40 and p["T"] > amb(p) + 3]
rows = []
for p in sel:
    m, w, pl, pir, a = dseval.run(p, 1.0, 0.8)
    rows.append(dict(lr=np.log(max(m - a, 1) / (p["T"] - a)), D=p["d"], H=p["h"], KV=p["kv"], I=p["a"], V=p["v"], prop=p["prop"], wash=w, PL=pl, PNL=pir, share=pir / (pir + pl), dur=p["dur"] or np.nan, amb=a, rise=p["T"] - a))
lr = np.array([r["lr"] for r in rows])
for k in ("D", "H", "KV", "I", "V", "prop", "wash", "PL", "PNL", "share", "dur", "amb", "rise"):
    v = np.array([r[k] for r in rows], float); ok = ~np.isnan(v)
    print("%-6s rho %+.2f (n %d)" % (k, spearmanr(v[ok], lr[ok])[0], ok.sum()))
for lo, hi in ((0, 60), (60, 61), (61, 125), (125, 1e9)):
    k = np.array([lo <= (r["dur"] if not np.isnan(r["dur"]) else -1) < hi for r in rows])
    if k.sum(): print("dur %s-%s s: n %d ratio %.2f" % (lo, hi, k.sum(), np.exp(np.median(lr[k]))))
