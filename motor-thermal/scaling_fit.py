#!/usr/bin/env python3
"""Firmware motor_thermal.c replica with stator / pole scaling knobs, fitted on the 5" and 3" bench sets.

  scaling_fit.py check            replica on the 5" sets with the current firmware laws (RMSE per set)
  scaling_fit.py fit [prop|noprop] fit the scaling exponents on the 3" packet (5" sets unchanged by construction)

Scaling from the 2203.5 / 2800 KV / 14-magnet reference (s = stator, D diameter, H height, P magnets):
  kCu  = REF_K_CU·(H/3.5)·(D/22)^alpha·KV²            ripple copper loss (firmware today: alpha = 0)
  aIron= REF_A_IRON·mass·(P/14)^beta                    iron loss ∝ electrical frequency^beta at the same rpm
  rsc  = REF_RSC/(DH ratio)^gamma, rca = REF_RCA/area^gamma   (firmware today: gamma = 1)
  C    = REF_C·mass^delta                               (firmware today: delta = 1)
"""
import csv, sys
import numpy as np
from scipy.optimize import least_squares

sys.path.insert(0, ".")
from thermal_model import load, col, camera

REF = dict(D=22.0, H=3.5, KV=2800.0, P=14, CS=2.88, CC=21.9, RSC=3.63, RCA=11.3, AI=0.0449, KCU=7.94e-07, RLL=0.2)
AIR_GAIN, PROP_VI_K, DT = 1.56, 0.252, 0.05
AUTO_PROP = [(9, 1.6), (12, 2.5), (15, 3.0), (19, 3.5), (22, 4.5), (23, 5.0), (27, 5.5), (30, 7.0), (999, 8.0)]
HF80_GAIN = 30.9 / 35.3


class Motor:
    def __init__(self, d, h, kv, poles, prop_inch):
        self.d, self.h, self.kv, self.poles = d, h, kv, poles
        self.prop = prop_inch if prop_inch is not None else next(p for m, p in AUTO_PROP if d <= m)

    def params(self, alpha=0.0, beta=0.0, gamma=1.0, delta=1.0, g=(1, 1, 1, 1), phi=0.0, air0=1.0, hc=None, pD=None, pH=None):
        """g: global multipliers on (kCu, aIron, R, C) applied to every motor (reference refit)."""
        d, h = self.d, self.h
        mass = d * d * h / (REF["D"] ** 2 * REF["H"])
        area = (d * h + 0.5 * d * d) / (REF["D"] * REF["H"] + 0.5 * REF["D"] ** 2)
        dh = d * h / (REF["D"] * REF["H"])
        cm = mass ** delta if hc is None else d * d * (h + hc) / (REF["D"] ** 2 * (REF["H"] + hc))   # heat capacity ratio
        bell = (0.5 * d + 3) * 1e-3
        return dict(
            cs=REF["CS"] * cm * g[3], cc=REF["CC"] * cm * g[3],
            rsc=REF["RSC"] / dh ** gamma * g[2], rca=REF["RCA"] / area ** gamma * g[2],
            ai=REF["AI"] * 1e-6 * (mass if pD is None else (d / REF["D"]) ** pD * (h / REF["H"]) ** pH)
               * (self.poles / REF["P"]) ** beta * g[1],
            kcu=REF["KCU"] * (h / REF["H"]) * (d / REF["D"]) ** alpha * (self.kv * 1e-3) ** 2 * g[0],
            rll=REF["RLL"] * (REF["KV"] / self.kv) ** 2 * (REF["H"] / h),
            phi=phi, air0=air0,
            airk=((2 * np.pi * bell) ** 2 + (self.prop * 0.0254 * PROP_VI_K) ** 2) / 3600.0)


def simulate(p, s):
    """s: dict t, rpm, vbat, idx, amps, cmd, Tamb, T0 -> windings temperature at the sample times."""
    t = s["t"]
    tt = np.arange(t[0], t[-1], DT)
    rpm = np.interp(tt, t, s["rpm"]); vb = np.interp(tt, t, s["vbat"]); idx = np.interp(tt, t, s["idx"])
    amps = np.interp(tt, t, s["amps"]); cmd = np.interp(tt, t, s["cmd"])
    pcu = p["kcu"] * (vb * idx) ** 2
    pir = p["ai"] * rpm ** 2
    v = np.sqrt(rpm ** 2 * p["airk"] + s.get("vext", 0.0) ** 2) * 0.1     # vext: air from outside (craft speed; bench: cooling prop)
    air = p["air0"] + AIR_GAIN * v ** 1.5 + s.get("aext", 0.0)     # aext: bench cooling prop on another motor (additive, rest-dominated)
    iload = np.maximum(amps - pcu / np.maximum(vb, 1), 0)
    pload = np.minimum(p["rll"] * (iload / np.maximum(cmd, 0.05)) ** 2, amps * vb)
    ts = tc = s["T0"]; ta = s["Tamb"]
    out = np.empty_like(tt)
    a1, a2 = DT / p["cs"], DT / p["cc"]
    for i in range(len(tt)):
        q = (ts - tc) / p["rsc"]
        ts += (pcu[i] + pload[i] + p["phi"] * pir[i] - q) * a1
        tc += ((1 - p["phi"]) * pir[i] + q - (tc - ta) * air[i] / p["rca"]) * a2
        out[i] = ts
    return np.interp(t, tt, out)


def five_inch_sets():
    cam = load("logs/rand1_cam.csv")
    sets = []
    for name in ("rand1", "rand2", "flight1", "flight2"):
        b = load(f"logs/{name}.csv")
        t = col(b, "unix"); idx = np.nan_to_num(col(b, "hf80", 0.0))
        quiet = np.array([x["noise"] == "none" for x in b]); idx[quiet] = 0.0
        tc, Tc, _ = camera(cam, t[0] - 2, t[-1] + 2)
        T = np.interp(t, tc, Tc)
        sets.append(dict(name=name, t=t, rpm=np.nan_to_num(col(b, "rpm", 0.0)), vbat=np.nan_to_num(col(b, "vbat"), nan=16.0),
                         idx=idx, amps=np.clip(np.nan_to_num(col(b, "amps_adc", 0.0)) - 0.20, 0, None),
                         cmd=np.clip((np.nan_to_num(col(b, "base", 1000.0)) - 1000) / 1000, 0, 1),
                         T=T, T0=T[0], Tamb=30.0))   # harness ambient (gyro stub), room ~29-30 °C then
    return sets


def three_inch_set(tamb=25.4):
    rows = []
    for name in ("a_base1150", "b_s20_201", "c_s30_201"):
        rows += list(csv.DictReader(open(f"logs/m3in_{name}.csv")))
    t = np.array([float(x["unix"]) for x in rows])
    run = np.array([x["state"] == "run" for x in rows]) & (np.array([float(x["cmd_mean"]) for x in rows]) > 1000)
    rms = np.array([float(x["rms"]) for x in rows]) * run
    rpm = np.array([float(x["rpm"]) if x["rpm"] else 0.0 for x in rows])
    vbat = np.array([float(x["vbat"]) for x in rows])
    cmd4 = np.array([float(x["cmd_mean"]) for x in rows]) / 1000 - 1
    share = cmd4 ** 2 / (cmd4 ** 2 + 0.1 ** 2)                      # motor 1 holds 1100: battery current split by command²
    amps = np.clip(np.array([float(x["amps"]) for x in rows]) - 0.26, 0, None) * share
    cam = load("logs/m3in_pack_cam.csv")
    tc, Tc, _ = camera(cam, t[0] - 2, t[-1] + 2)
    T = np.interp(t, tc, Tc)
    return dict(name="3in", t=t, rpm=rpm, vbat=vbat, idx=HF80_GAIN * rms, amps=amps, cmd=cmd4, T=T, T0=T[0], Tamb=tamb)


M5 = lambda prop: Motor(22, 3.5, 2800, 14, prop)
M3 = lambda prop: Motor(14, 8.0, 3500, 12, prop)


def rmse(p, s):
    e = simulate(p, s) - s["T"]
    return float(np.sqrt(np.mean(e ** 2))), float(np.max(np.abs(e)))


def report(tag, kw, prop, s5, s3):
    out = [f"{tag}:"]
    for s in s5:
        out.append("%s %.2f/%.1f" % (s["name"], *rmse(M5(prop).params(**kw), s)))
    out.append("3in %.2f/%.1f" % rmse(M3(prop).params(**kw), s3))
    print("  ".join(out) + "   (RMSE/max °C)")


def main():
    mode = sys.argv[1]
    prop = 0.0 if (len(sys.argv) > 2 and sys.argv[2] == "noprop") else None
    s5, s3 = five_inch_sets(), three_inch_set()
    report("firmware today", {}, prop, s5, s3)
    if mode != "fit":
        return
    names = ["alpha", "beta", "gamma", "delta"]
    phys = dict(alpha=2.0, beta=2.0, gamma=1.0, delta=1.0)
    report("physics prior (D², poles²)", phys, prop, s5, s3)

    def res3(x):
        return simulate(M3(prop).params(**dict(zip(names, x))), s3) - s3["T"]
    r = least_squares(res3, [2.0, 2.0, 1.0, 1.0], bounds=([0, 0, 0, 0], [4, 3, 3, 2]))
    fit = dict(zip(names, r.x))
    print("fit exponents:", "  ".join(f"{k} {v:.2f}" for k, v in fit.items()))
    report("fitted exponents", fit, prop, s5, s3)

    # joint: exponents + global multipliers on (kCu, aIron, R, C), all sets weighted by sample count
    def resj(x):
        kw = dict(zip(names, x[:4])); kw["g"] = tuple(x[4:])
        return np.concatenate([simulate(M5(prop).params(**kw), s) - s["T"] for s in s5]
                              + [simulate(M3(prop).params(**kw), s3) - s3["T"]])
    rj = least_squares(resj, list(r.x) + [1, 1, 1, 1], bounds=([0, 0, 0, 0, .3, .3, .3, .3], [4, 3, 3, 2, 3, 3, 3, 3]))
    kw = dict(zip(names, rj.x[:4])); kw["g"] = tuple(rj.x[4:])
    print("joint:", "  ".join(f"{k} {v:.2f}" for k, v in zip(names + ["g_kcu", "g_iron", "g_R", "g_C"], rj.x)))
    report("joint refit", kw, prop, s5, s3)


if __name__ == "__main__":
    main()
