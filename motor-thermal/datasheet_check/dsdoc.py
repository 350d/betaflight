"""Tables for DATASHEET_VALIDATION.md: per brand, before / after (vk 1.0, n 0.8), exponent check."""
import sys, os, types, numpy as np
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S); sys.argv = [sys.argv[0], sys.argv[1]]
_src = open(os.path.join(S, "dseval.py")).read(); _src = _src[:_src.index('if os.environ.get("DSEVAL_LIB")')]
dseval = types.ModuleType("dseval"); dseval.__file__ = os.path.join(S, "dseval.py"); exec(compile(_src, "dseval.py", "exec"), dseval.__dict__)
P = [p for p in dseval.P if dseval.run(p, 1.0, 0.8)[1] < 40 and p["T"] > (p["amb"] if p["amb"] is not None else 25) + 3]
def lr(p, vk, n):
    m, w, pl, pir, amb = dseval.run(p, vk, n); return np.log(max(m - amb, 1.0) / max(p["T"] - amb, 3.0)), w
def q(e): return "%.2f (%.2f-%.2f)" % (np.exp(np.median(e)), np.exp(np.percentile(e, 25)), np.exp(np.percentile(e, 75)))
METHOD = {"T-Motor": "motor surface, 1 min at 100 %", "BrotherHobby": "inside copper wire, 10 s-2 min at 100 %", "RCinPower": "3-5 s at 100 %", "GEPRC": "not stated / 3-15 s, one 10 min", "iFlight": "not stated (1 min on some)", "VCI": "shaft, after a 0-100 % sweep", "Axisflying": "not stated", "AMAX": "10 s at 100 %", "Flywoo": "3 s at 100 %"}
print("| brand | points | how the temperature was taken | rise ratio before | rise ratio now |")
print("|---|---|---|---|---|")
for b in sorted(set(p["brand"] for p in P), key=lambda b: -sum(1 for p in P if p["brand"] == b)):
    sel = [p for p in P if p["brand"] == b]
    print("| %s | %d | %s | %s | %s |" % (b, len(sel), METHOD.get(b, ""), q([lr(p, 0, 1)[0] for p in sel]), q([lr(p, 1.0, 0.8)[0] for p in sel])))
print("| **all** | **%d** | | **%s** | **%s** |" % (len(P), q([lr(p, 0, 1)[0] for p in P]), q([lr(p, 1.0, 0.8)[0] for p in P])))
print()
print("| wash speed | points | n 0.8 | n 2 | n 4 |")
print("|---|---|---|---|---|")
from scipy.optimize import least_squares
fits = {n: least_squares(lambda x: np.array([lr(p, x[0], n)[0] for p in P]), [1.0], bounds=([0.05], [30])).x[0] for n in (0.8, 2.0, 4.0)}
for lo, hi in ((0, 8), (8, 12), (12, 16), (16, 25), (25, 40)):
    sel = [p for p in P if lo <= lr(p, 1.0, 0.8)[1] < hi]
    if sel:
        print("| %d-%d m/s | %d | %s |" % (lo, hi, len(sel), " | ".join("%.2f" % np.exp(np.median([lr(p, fits[n], n)[0] for p in sel])) for n in (0.8, 2.0, 4.0))))
print("\n(vk refitted per exponent on all points: %s)" % ", ".join("n %.1f: %.2f m/s" % (n, v) for n, v in fits.items()))
