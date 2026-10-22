"""Size/KV law of the line-to-line winding resistance and of the no-load loss from the published datasheet values.
Usage: rlaw_fit.py datasheets/motor_datasheets.json"""
import sys, json, numpy as np
M = []
for r in json.load(open(sys.argv[1])):
    d, h, kv = r.get("stator_diameter_mm"), r.get("stator_height_mm"), r.get("kv")
    if d and h and kv and 8 <= d <= 32 and h <= 12:                     # FPV-size motors
        M.append(dict(brand=r["brand"], d=float(d), h=float(h), kv=float(kv), r=r.get("internal_resistance_mohm") or 0,
                      ia=r.get("idle_current_a") or 0, iv=r.get("idle_current_voltage_v") or 0))
def fit(X, y):
    A = np.column_stack([np.ones(len(y))] + X); c = np.linalg.lstsq(A, y, rcond=None)[0]; return c, y - A @ c
def show(tag, res):
    a = np.abs(res)
    print("  %-46s n=%4d median x%.2f  90%% within x%.2f" % (tag, len(res), np.exp(np.median(a)), np.exp(np.percentile(a, 90))))
R = [m for m in M if 3 <= m["r"] <= 5000]
old = lambda m: 70.0 * (2800 / m["kv"]) ** 2 / ((m["h"] / 3.5) * (m["d"] / 22) ** 2.5)
new = lambda m: 70.0 * (2800 / m["kv"]) ** 2 / ((m["h"] / 3.5) ** 1.29 * (m["d"] / 22) ** 3.95)
print("winding resistance (mOhm), reference 70 mOhm at 2203.5 2800KV:")
show("previous firmware R ~ 1/(KV^2 H D^2.5)", np.array([np.log(m["r"] / old(m)) for m in R]))
show("firmware R ~ 1/(KV^2 H^1.29 D^3.95)", np.array([np.log(m["r"] / new(m)) for m in R]))
c, res = fit([np.log([m["d"] for m in R]), np.log([m["h"] for m in R])], np.array([np.log(m["r"] * m["kv"] ** 2) for m in R]))
show("free fit with KV^-2: D^%.2f H^%.2f" % (c[1], c[2]), res)
for lo, hi in ((0, 14), (15, 20), (21, 25), (26, 99)):
    s = [m for m in R if lo <= m["d"] <= hi]
    if s: print("  D %2d-%2d mm: datasheet / law, previous %.2f, firmware %.2f (n %d)" % (lo, hi, np.exp(np.median([np.log(m["r"] / old(m)) for m in s])), np.exp(np.median([np.log(m["r"] / new(m)) for m in s])), len(s)))
by = {}
for m in R: by.setdefault(m["brand"], []).append(np.log(m["r"] / new(m)))
print("  brand bias datasheet / law (n >= 8):", ", ".join("%s %.2f" % (b, np.exp(np.median(v))) for b, v in sorted(by.items()) if len(v) >= 8))
N = [m for m in M if m["ia"] > 0 and m["iv"] > 0 and m["ia"] <= 10]
cn = lambda m: m["iv"] * m["ia"] / (m["kv"] * m["iv"] * 0.95 / 1000) ** 2
c, res = fit([np.log([m["d"] for m in N]), np.log([m["h"] for m in N]), np.log([m["kv"] for m in N])], np.array([np.log(cn(m)) for m in N]))
print("no-load power at the idle-current voltage (W per krpm^2):")
show("fit %.3g D^%.2f H^%.2f KV^%.2f" % (np.exp(c[0]), c[1], c[2], c[3]), res)
