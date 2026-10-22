"""Replay the flight log through host builds of motor_thermal.c (eval4) and compare motor 2 with the NTC.
Usage: ntc_replay.py log.bbl out_dir EVAL_OLD EVAL_NEW"""
import os, subprocess, sys
import numpy as np
from orangebox import Parser

log, OUT, EVAL_OLD, EVAL_NEW = sys.argv[1:5]
D, H, KV, POLES = 22, 35, 2800, 14
p = Parser.load(log)
names = p.field_names; ix = {n: i for i, n in enumerate(names)}
cols = ["time", "vbatLatest", "amperageLatest", "rssi"] + ["motor[%d]" % i for i in range(4)] + ["eRPM[%d]" % i for i in range(4)] + ["debug[%d]" % i for i in range(4)]
a = np.array([[fr.data[ix[c]] for c in cols] for fr in p.frames() if len(fr.data) == len(names)], float)
t = (a[:, 0] - a[0, 0]) * 1e-6
keep = np.r_[True, np.diff(t) > 0]; a, t = a[keep], t[keep]
fs = 1 / np.median(np.diff(t))
# noise index from the logged motor command (log rate ~1 kHz, so it misses content above fs/2): PT1 highpass 80 Hz, EMA 0.5 s of the square
cmd_raw = (a[:, 4:8] - 48) / 1999 * 1000                     # motor units 0..1000 like the firmware index
dt = 1 / fs; k_hp = 1 / (1 + 2 * np.pi * 80 * dt); k_ema = dt / 0.5
idx = np.zeros_like(cmd_raw); y = np.zeros(4); xp = cmd_raw[0].copy(); ms = np.zeros(4)
for n in range(len(t)):
    y = k_hp * (y + cmd_raw[n] - xp); xp = cmd_raw[n]
    ms += k_ema * (y * y - ms); idx[n] = np.sqrt(ms)
step = max(1, int(round(0.01 * fs)))
a, t, idx = a[::step], t[::step], idx[::step]
vbat, amps = a[:, 1] * 0.01, a[:, 2] * 0.01 * float(os.environ.get("AMPS_K", "1"))
cmd = np.clip((a[:, 4:8] - 48) / 1999, 0, 1); rpm = a[:, 8:12] * 100 / (POLES / 2)
v = np.clip(a[:, 3], 1, 1022); r = 3300 * v / (1023 - v)
ntc = 1 / (1 / 298.15 + np.log(r / 10000) / float(os.environ.get("NTC_B", "3950"))) - 273.15
onboard = a[:, 12:16] / 10
def run(ev, tamb, t0, extra_env=None):
    path = os.path.join(OUT, "ntc_fl4.csv")
    with open(path, "w") as f:
        f.write("t,vbat,amps,armed,c0,c1,c2,c3,r0,r1,r2,r3,i0,i1,i2,i3\n")
        for k in range(len(t)):
            f.write("%.3f,%.3f,%.3f,1," % (t[k], vbat[k], amps[k]) + ",".join("%.4f" % x for x in cmd[k]) + "," + ",".join("%.0f" % x for x in rpm[k]) + "," + ",".join("%.2f" % x for x in idx[k]) + "\n")
    env = dict(os.environ, **(extra_env or {}))
    o = subprocess.run([ev, path, str(D), str(H), str(KV), str(POLES), "0", "%.2f" % tamb, "%.2f" % t0, "0.0", "1"], capture_output=True, text=True, check=True, env=env).stdout.split()
    return np.array(o, float).reshape(-1, 5)[:, 1:]
amb_ntc = ntc[: max(1, int(3 / 0.01))].mean()
amb_fc = onboard[0, 1]
print("log %.0f s at %.0f Hz | vbat %.1f-%.1f V | battery current mean %.1f A max %.1f | motor 2 cmd mean %.2f | idx m2 mean %.1f | rpm m2 mean %.0f" % (
    t[-1], fs, vbat.min(), vbat.max(), amps.mean(), amps.max(), cmd[:, 1].mean(), idx[:, 1].mean(), rpm[:, 1].mean()))
print("ambient: NTC at start %.1f C, on-board model start %.1f C" % (amb_ntc, amb_fc))
old_same = run(EVAL_OLD, amb_fc, amb_fc)
old_ntc = run(EVAL_OLD, amb_ntc, amb_ntc)
new_ntc = run(EVAL_NEW, amb_ntc, amb_ntc)
print("replay check: FC firmware on host vs on-board debug, motor 2: max |diff| %.1f K" % np.max(np.abs(old_same[:, 1] - onboard[:, 1])))
def rise(x): return x - x[0]
rows = [("NTC (motor 2)", ntc), ("on-board FC model (old fw, ambient from the board)", onboard[:, 1]),
        ("FC firmware, ambient from NTC", old_ntc[:, 1]), ("current firmware (prop wash), ambient from NTC", new_ntc[:, 1])]
for name, x in rows:
    print("%-52s start %5.1f  peak %5.1f (rise %+5.1f K)  end %5.1f" % (name, x[0], x.max(), x.max() - x[0], x[-1]))
for lo in range(0, int(t[-1]) + 1, 20):
    k = (t >= lo) & (t < lo + 20)
    print("t %3d-%3d  NTC %5.1f | old fw %5.1f | new fw %5.1f | thr m2 %.2f  amps %5.1f  rpm %5.0f" % (lo, lo + 20, ntc[k].mean(), old_ntc[k, 1].mean(), new_ntc[k, 1].mean(), cmd[k, 1].mean(), amps[k].mean(), rpm[k, 1].mean()))
np.savez(os.path.join(OUT, "ntc_replay.npz"), t=t, ntc=ntc, onboard=onboard, old_ntc=old_ntc, new_ntc=new_ntc, cmd=cmd, amps=amps, rpm=rpm, idx=idx, vbat=vbat)
