"""Model vs ~260 datasheet temperature points (motor_datasheets.json). Python replica of the firmware with the prop-wash term."""
import sys, os, numpy as np
from scipy.optimize import least_squares
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
import model, dsdb
P = dsdb.points(sys.argv[1])
FW = [1.0]
def run(pt, vk, n, dt=0.05):
    d, h, kv = pt["d"], pt["h"], pt["kv"]
    p = model.params(d, int(round(h * 10)), kv, pt["poles"], int(round(pt["prop"] * 10)), pt["r_mohm"])
    u = kv * pt["v"] * pt["thr"]
    rpm = pt["rpm"] if pt["rpm"] else u * (-5.44e-6 * u + 0.944)
    amb = pt["amb"] if pt["amb"] is not None else 25.0
    dur = pt["dur"] if pt["dur"] else 60.0
    v = np.sqrt(rpm ** 2 * p["airk"]); air = 1 + p["gain"] * (v * 0.1) ** p["n"]
    wash = rpm / 60 * p["propm"] * 0.252
    b = min((wash / vk) ** n, 100.0) if vk > 0 else 0.0
    pir = p["ai"] * rpm ** 2; pl = min(p["rll"] * (pt["a"] / max(pt["thr"], 0.05)) ** 2, pt["a"] * pt["v"])
    ts = tc = amb
    for _ in range(int(dur / dt)):
        q = (ts - tc) / p["rsc"]
        ts += (pl - q - (ts - amb) * FW[0] * b / p["rca"]) * dt / p["cs"]
        tc += (pir + q - (tc - amb) * (air + b) / p["rca"]) * dt / p["cc"]
    return (ts if pt["kind"] != "surface" else tc), wash, pl, pir, amb
def group(pt):
    if pt["dur"] is None: return "no duration"
    if pt["dur"] < 30: return "short (<30 s)"
    return "%s %s" % (pt["brand"] if pt["brand"] in ("T-Motor", "BrotherHobby", "GEPRC") else "other", pt["kind"])
def stats(vk, n, sel):
    res = {}
    for pt in sel:
        m, w, pl, pir, amb = run(pt, vk, n)
        res.setdefault(group(pt), []).append((m - pt["T"], w, m - amb, pt["T"] - amb))
    return res
def show(tag, vk, n):
    print("== %s (vk %.2f m/s, n %.2f)" % (tag, vk, n))
    allr = []
    for g, r in sorted(stats(vk, n, P).items()):
        e = np.array([x[0] for x in r]); allr += r
        print("   %-28s n=%3d  bias %+6.1f  RMSE %6.1f  median |err| %5.1f" % (g, len(e), e.mean(), np.sqrt(np.mean(e ** 2)), np.median(np.abs(e))))
    e = np.array([x[0] for x in allr]); w = np.array([x[1] for x in allr]); ratio = np.array([x[2] / max(x[3], 3) for x in allr])
    for lo, hi in ((0, 8), (8, 12), (12, 16), (16, 25), (25, 99)):
        k = (w >= lo) & (w < hi)
        if k.sum(): print("   wash %2d-%2d m/s: n=%3d bias %+6.1f, median rise ratio model/datasheet %.2f" % (lo, hi, k.sum(), e[k].mean(), np.median(ratio[k])))
if os.environ.get("DSEVAL_LIB"): raise SystemExit
TRAIN = [pt for pt in P if pt["brand"] in ("T-Motor", "BrotherHobby") and pt["dur"] and pt["dur"] >= 60 and pt["amb"] is not None]
TEST = [pt for pt in P if pt not in TRAIN]
print("points: %d, train (T-Motor + BrotherHobby, >= 1 min, ambient given) %d, test %d" % (len(P), len(TRAIN), len(TEST)))
show("firmware before the wash term", 0, 1)
show("current choice", 0.5, 0.8)
for n in (0.8, 2.0, 4.0):
    def res(x): return np.array([(run(pt, x[0], n)[0] - pt["T"]) for pt in TRAIN])
    r = least_squares(res, [3.0], bounds=([0.05], [60]))
    e_tr = res(r.x); e_te = np.array([(run(pt, r.x[0], n)[0] - pt["T"]) for pt in TEST])
    print("## n %.1f: vk fitted %.2f m/s | train RMSE %.1f bias %+.1f | test RMSE %.1f bias %+.1f median |err| %.1f" % (n, r.x[0], np.sqrt(np.mean(e_tr ** 2)), e_tr.mean(), np.sqrt(np.mean(e_te ** 2)), e_te.mean(), np.median(np.abs(e_te))))
    show("n %.1f refit" % n, r.x[0], n)
