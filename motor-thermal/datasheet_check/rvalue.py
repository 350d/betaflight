"""Does the datasheet R still add accuracy once the size law is refitted? Variants of the R fed to the firmware replica."""
import sys, os, types, numpy as np
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
DS = sys.argv[1]; sys.argv = [sys.argv[0], DS]
_src = open(os.path.join(S, "dseval.py")).read(); _src = _src[:_src.index('if os.environ.get("DSEVAL_LIB")')]
dseval = types.ModuleType("dseval"); dseval.__file__ = os.path.join(S, "dseval.py"); exec(compile(_src, "dseval.py", "exec"), dseval.__dict__)
amb = lambda p: p["amb"] if p["amb"] is not None else 25.0
law = lambda p: 70 * (2800 / p["kv"]) ** 2 / ((p["h"] / 3.5) ** 1.29 * (p["d"] / 22) ** 3.95)
P = [p for p in dseval.P if p["r_mohm"] and dseval.run(p, 1.0, 0.8)[1] < 40 and p["T"] > amb(p) + 3]
brands = {}
for p in P: brands.setdefault(p["brand"], []).append(np.log(p["r_mohm"] / law(p)))
bm = {b: np.exp(np.median(v)) for b, v in brands.items()}
VAR = {"law": lambda p: law(p), "datasheet": lambda p: p["r_mohm"], "sqrt(datasheet*law)": lambda p: np.sqrt(p["r_mohm"] * law(p)),
       "datasheet / brand bias": lambda p: p["r_mohm"] / bm[p["brand"]]}
G = {"BrotherHobby windings": lambda p: p["brand"] == "BrotherHobby" and p["kind"] == "windings",
     "RCinPower windings": lambda p: p["brand"] == "RCinPower", "T-Motor surface": lambda p: p["brand"] == "T-Motor" and p["kind"] == "surface",
     "unspecified (lower bound)": lambda p: p["brand"] not in ("BrotherHobby", "RCinPower", "T-Motor")}
for g, f in G.items():
    sel = [p for p in P if f(p)]
    print("== %s, n=%d" % (g, len(sel)))
    for v, rf in VAR.items():
        lr = np.array([np.log(max(dseval.run(dict(p, r_mohm=rf(p)), 1.0, 0.8)[0] - amb(p), 1.0) / (p["T"] - amb(p))) for p in sel])
        de = (np.exp(lr) - 1) * np.array([p["T"] - amb(p) for p in sel])
        c = lr - np.median(lr)
        print("   %-24s ratio %.2f (IQR %.2f-%.2f) |err| median %4.1f °C | spread after centring: median |x| %.2f" % (v, np.exp(np.median(lr)), *np.exp(np.percentile(lr, [25, 75])), np.median(np.abs(de)), np.exp(np.median(np.abs(c)))))
print("brand R bias (datasheet/law):", ", ".join("%s %.2f" % (b, v) for b, v in sorted(bm.items())))
