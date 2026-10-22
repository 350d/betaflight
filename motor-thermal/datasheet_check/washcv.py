import sys, os, types, numpy as np
from scipy.optimize import least_squares
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
DS = sys.argv[1]; sys.argv = [sys.argv[0], DS]
_src = open(os.path.join(S, "dseval.py")).read(); _src = _src[:_src.index('if os.environ.get("DSEVAL_LIB")')]
dseval = types.ModuleType("dseval"); dseval.__file__ = os.path.join(S, "dseval.py"); exec(compile(_src, "dseval.py", "exec"), dseval.__dict__)
amb = lambda p: p["amb"] if p["amb"] is not None else 25.0
law = lambda p: 70 * (2800 / p["kv"]) ** 2 / ((p["h"] / 3.5) ** 1.29 * (p["d"] / 22) ** 3.95)
ALL = [dict(p, r_mohm=law(p)) for p in dseval.P if dseval.run(p, 1.0, 0.8)[1] < 40 and p["T"] > amb(p) + 3]
BH = [p for p in ALL if p["brand"] == "BrotherHobby" and p["kind"] == "windings"]
LOW = [p for p in ALL if p["brand"] not in ("BrotherHobby", "RCinPower", "T-Motor")]
lr = lambda p, vk, n: np.log(max(dseval.run(p, vk, n)[0] - amb(p), 1.0) / (p["T"] - amb(p)))
def rep(tag, vk, n):
    e = np.array([lr(p, vk, n) for p in BH]); el = np.array([lr(p, vk, n) for p in LOW])
    de = (np.exp(e) - 1) * np.array([p["T"] - amb(p) for p in BH])
    print("%-34s BH n=%d ratio %.2f IQR %.2f-%.2f |err| %.1f °C RMS log %.3f | unspecified ratio %.2f below bound %d %%" % (tag, len(BH), np.exp(np.median(e)), *np.exp(np.percentile(e, [25, 75])), np.median(np.abs(de)), np.sqrt(np.mean(e ** 2)), np.exp(np.median(el)), 100 * np.mean(el < 0)))
rep("firmware vk 1.0, n 0.8", 1.0, 0.8)
r = least_squares(lambda x: [lr(p, x[0], x[1]) for p in BH], [1.0, 0.8], bounds=([0.05, 0.2], [30, 3])); rep("fit on BH: vk %.2f n %.2f" % tuple(r.x), *r.x)
r1 = least_squares(lambda x: [lr(p, x[0], 0.8) for p in BH], [1.0], bounds=([0.05], [30])); rep("fit vk only: vk %.2f" % r1.x[0], r1.x[0], 0.8)
motor = lambda p: (p["name"].split(" ")[1], p["kv"])
ms = sorted(set(motor(p) for p in BH)); err0, err1 = [], []
for k in range(5):
    te = set(ms[k::5]); tr = [p for p in BH if motor(p) not in te]
    x = least_squares(lambda x: [lr(p, x[0], x[1]) for p in tr], [1.0, 0.8], bounds=([0.05, 0.2], [30, 3])).x
    for p in BH:
        if motor(p) in te: err0.append(lr(p, 1.0, 0.8)); err1.append(lr(p, *x))
print("5-fold by motor: RMS log firmware %.3f -> refit %.3f (n %d)" % (np.sqrt(np.mean(np.square(err0))), np.sqrt(np.mean(np.square(err1))), len(err0)))
