"""NTC on motor 2 (blackbox rssi = ADC/4) vs the on-board model (debug MOTOR_THERMAL, 0.1 °C). Usage: ntc_flight.py log.bbl out_prefix"""
import math, sys
import numpy as np
from orangebox import Parser

B, RPULL, R25 = 3950.0, 3300.0, 10000.0
path, out = sys.argv[1], sys.argv[2]
p = Parser.load(path)
names = p.field_names
want = ["time", "rssi", "vbat", "amperage", "rcCommand[3]"] + ["debug[%d]" % i for i in range(4)] + \
       ["motor[%d]" % i for i in range(4)] + ["eRPM[%d]" % i for i in range(4)]
idx = [names.index(n) for n in want if n in names]
have = [n for n in want if n in names]
rows = [[fr.data[i] for i in idx] for fr in p.frames() if len(fr.data) == len(names)]
d = np.array(rows, dtype=float)
col = {n: d[:, k] for k, n in enumerate(have)}
t = (col["time"] - col["time"][0]) * 1e-6
step = max(1, int(round(0.02 / np.median(np.diff(t)))))           # 50 Hz
sl = slice(None, None, step)
v = np.clip(col["rssi"][sl], 1, 1022)
r = RPULL * v / (1023 - v)
ntc = 1 / (1 / 298.15 + np.log(r / R25) / B) - 273.15
est = {i: col["debug[%d]" % i][sl] / 10 for i in range(4)}
np.savez(out + ".npz", t=t[sl], ntc=ntc, adc=v, est=np.array([est[i] for i in range(4)]),
         vbat=col.get("vbat", np.zeros_like(t))[sl], amp=col.get("amperage", np.zeros_like(t))[sl],
         thr=col.get("rcCommand[3]", np.zeros_like(t))[sl], motor=np.array([col["motor[%d]" % i][sl] for i in range(4)]),
         erpm=np.array([col["eRPM[%d]" % i][sl] for i in range(4)]) if "eRPM[0]" in col else np.zeros((4, len(t[sl]))))
print("fields:", have)
print("duration %.0f s, log rate %.0f Hz, headers: %s" % (t[-1], 1 / np.median(np.diff(t)), {k: p.headers.get(k) for k in ("Firmware revision", "looptime", "P interval", "debug_mode")}))
e2 = est[1]
for lo in range(0, int(t[sl][-1]) + 1, 20):
    k = (t[sl] >= lo) & (t[sl] < lo + 20)
    if k.any():
        print("t %3d-%3d s  NTC %5.1f  model m2 %5.1f  (m1 %5.1f m3 %5.1f m4 %5.1f)  thr %4.0f  amps %5.1f" % (
            lo, lo + 20, ntc[k].mean(), e2[k].mean(), est[0][k].mean(), est[2][k].mean(), est[3][k].mean(),
            col["rcCommand[3]"][sl][k].mean(), col["amperage"][sl][k].mean() / 100 if "amperage" in col else 0))
print("NTC start %.1f max %.1f end %.1f | model m2 start %.1f max %.1f end %.1f" % (ntc[:50].mean(), ntc.max(), ntc[-50:].mean(), e2[:50].mean(), e2.max(), e2[-50:].mean()))
