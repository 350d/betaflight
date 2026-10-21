"""Python replica of flight/motor_thermal.c (fork head 23a443516) with optional thermodynamic extensions, for the refit.
Extensions (all off by default = the firmware):
  rsa      windings -> air resistance at the reference (K/W, still air), Newton cooling with the same airflow factor, scaled 1/sqrt(area)
  a_iron   no-load loss coefficient at the reference, W/(rpm)^2 (firmware 0.0449e-6)
  conv_n   airflow exponent of the convection factor 1 + gain·(v/10)^n (firmware n 1.5, gain 1.56)
"""
import numpy as np
REF = dict(D=22.0, H=3.5, KV=2800.0, P=14.0, HC=12.0, CS=2.88, CC=21.9, RSC=3.63, RCA=11.3, AI=0.0449e-6, KCU=7.94e-07, RLL=0.070, GAIN=1.56, N=1.5, PVI=0.252)
AUTO = [(9, 16), (12, 25), (15, 30), (19, 35), (22, 45), (23, 50), (27, 55), (30, 70), (255, 80)]

def params(d, hdmm, kv, poles, prop_dinch=0, r_mohm=0, ref=REF, rsa=0.0, nsa=1.0):
    h = max(hdmm, 10) * 0.1; d = max(d, 5)
    dr, hr = d / ref["D"], h / ref["H"]
    area = (d * h + 0.5 * d * d) / (ref["D"] * ref["H"] + 0.5 * ref["D"] ** 2)
    hc = dr ** 2 * (h + ref["HC"]) / (ref["H"] + ref["HC"])
    kv = max(kv, 100); rs = hr * dr ** 2 * np.sqrt(dr)
    prop = 0 if prop_dinch < 0 else (prop_dinch or next(p for m, p in AUTO if d <= m))   # -1 = no prop (bench)
    bell = (0.5 * d + 3) * 1e-3; propm = prop * 0.1 * 0.0254
    return dict(cs=ref["CS"] * hc, cc=ref["CC"] * hc, rsc=ref["RSC"] / np.sqrt(dr * hr), rca=ref["RCA"] / np.sqrt(area),
                rsa=(rsa / np.sqrt(area)) if rsa > 0 else 0.0,
                ai=ref["AI"] * dr ** 0.15 * hr ** 0.25 * (max(poles, 2) / ref["P"]),
                kcu=ref["KCU"] * rs * (kv * 1e-3) ** 2, rll=(r_mohm * 1e-3) if r_mohm else ref["RLL"] * (ref["KV"] / kv) ** 2 / rs,
                airk=((2 * np.pi * bell) ** 2 + (propm * ref["PVI"]) ** 2) / 3600.0, propm=propm, gain=ref["GAIN"], n=ref["N"], nsa=nsa)

def simulate(p, t, rpm, vbat, idx, motor_amps, cmd, tamb, T0, vext=None, dt=0.05):
    """motor_amps: this motor's current (after idle and split); returns the windings temperature at t."""
    tt = np.arange(t[0], t[-1] + 1e-9, dt)
    rpm = np.interp(tt, t, rpm); vb = np.interp(tt, t, vbat); ix = np.interp(tt, t, idx); am = np.interp(tt, t, motor_amps); cm = np.interp(tt, t, cmd)
    ve = np.interp(tt, t, vext) if vext is not None else 0.0
    v = np.sqrt(rpm ** 2 * p["airk"] + ve ** 2) * 0.1
    air = 1.0 + p["gain"] * v ** p["n"]
    asa = v ** p["nsa"]                         # windings -> air: forced convection only (no still-air term)
    pcu = p["kcu"] * (vb * ix) ** 2
    pir = p["ai"] * rpm ** 2
    il = np.maximum(am - pcu / np.maximum(vb, 1.0), 0.0)
    pl = np.minimum(p["rll"] * (il / np.maximum(cm, 0.05)) ** 2, am * vb)
    ts = tc = T0; out = np.empty_like(tt)
    a1, a2, rsc, rca, rsa = dt / p["cs"], dt / p["cc"], p["rsc"], p["rca"], p["rsa"]
    for i in range(len(tt)):
        q = (ts - tc) / rsc
        qa = (ts - tamb) * asa[i] / rsa if rsa > 0 else 0.0
        ts += (pcu[i] + pl[i] - q - qa) * a1
        tc += (pir[i] + q - (tc - tamb) * air[i] / rca) * a2
        out[i] = ts
    return np.interp(t, tt, out)
