"""Datasheet temperature points from motor_datasheets.json -> model inputs."""
import json, re, numpy as np
def prop_inch(s):
    if not s: return None
    t = s.replace("×", "x").replace("*", "x").replace("X", "x")
    m = re.search(r"(\d+(?:\.\d+)?)\s*mm", t, re.I)
    if m: return float(m.group(1)) / 25.4
    m = re.search(r"(\d+(?:\.\d+)?)\s*x\s*\d", t)
    if m:
        v = float(m.group(1)); return v / 25.4 if v > 20 else v
    m = re.search(r"D\s?(\d{2,3})\b", t)
    if m: return float(m.group(1)) / 25.4
    m = re.search(r"(\d{4,5})", t)
    if m:
        v = float(m.group(1)[:2]) / 10.0
        return v if v >= 1.0 else None
    return None
def duration_s(s):
    if not s: return None
    t = s.lower()
    m = re.search(r"(\d+(?:\.\d+)?)\s*(?:min|mins|minute)", t)
    if m: return float(m.group(1)) * 60
    m = re.search(r"(\d+(?:\.\d+)?)\s*(?:s|sec|secs|second)\b", t)
    if m: return float(m.group(1))
    return None
def target_kind(rec, temp):
    lab = (temp.get("label_verbatim") or "") + " " + (rec.get("notes") or "")
    l = lab.lower()
    if "copper" in l or "winding" in l or "coil" in l: return "windings"
    if "surface" in l or "shaft" in l or "case" in l: return "surface"
    return "unspecified"
def points(path):
    out = []
    for rec in json.load(open(path)):
        d, h, kv = rec.get("stator_diameter_mm"), rec.get("stator_height_mm"), rec.get("kv")
        if not (d and h and kv): continue
        for tp in rec.get("temperature") or []:
            if tp.get("value_c") is None: continue
            prop = prop_inch(tp.get("prop")); v = tp.get("voltage_v"); thr = tp.get("throttle_pct") or 100
            if not (prop and v): continue
            row = None
            for pt in rec.get("prop_tests") or []:
                if prop_inch(pt.get("prop")) and abs(prop_inch(pt.get("prop")) - prop) < 0.05 and pt.get("voltage_v") and abs(pt["voltage_v"] - v) < 0.6:
                    rows = [r for r in pt.get("rows") or [] if r.get("current_a") and r.get("throttle_pct")]
                    if rows:
                        row = min(rows, key=lambda r: abs(r["throttle_pct"] - thr))
            if not row or abs(row["throttle_pct"] - thr) > 5: continue
            steps = sorted([(r["throttle_pct"] / 100.0, float(r["current_a"]), r.get("rpm")) for r in rows if r["throttle_pct"] < row["throttle_pct"]]) if row else []
            out.append(dict(steps=steps, name="%s %s %sKV %s %sV" % (rec["brand"], rec["model"], kv, tp.get("prop"), v), brand=rec["brand"],
                            d=float(d), h=float(h), kv=float(kv), poles=rec.get("magnets") or 14, r_mohm=rec.get("internal_resistance_mohm") or 0,
                            prop=prop, v=float(v), a=float(row["current_a"]), rpm=row.get("rpm"), thr=thr / 100.0, dur=duration_s(tp.get("duration")),
                            amb=tp.get("ambient_c"), T=float(tp["value_c"]), i0_a=rec.get("idle_current_a"), i0_v=rec.get("idle_current_voltage_v"), kind=target_kind(rec, tp), notes=rec.get("notes") or ""))
    return out
if __name__ == "__main__":
    import sys, collections
    p = points(sys.argv[1])
    print(len(p), "usable points")
    print(collections.Counter((x["brand"], x["kind"], "dur" if x["dur"] else "no dur") for x in p).most_common())
