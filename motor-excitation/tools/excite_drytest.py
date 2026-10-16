#!/usr/bin/env python3
"""Dry test of the excitation state machine over MSP, battery OFF (ESCs unpowered): checks start/stop,
keepalive timeout, parameter rejection, chirp end, clipping report, handover between sessions."""
import sys, time
from msp import Msp, MspError

m = Msp(sys.argv[1])
ok = True
def check(name, cond, info=""):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name, info)

def run_for(sec, session, ka=True):
    t = time.time()
    while time.time() - t < sec:
        if ka: m.excite_keepalive(session)
        time.sleep(0.1)

# 1. sine start / status / stop
m.excite_start(101, 0b0001, "sine", 1150, 50, 201, duration=30, ramp=0.5)
run_for(3.5, 101)   # ramp 0.5 s + 6 RMS time constants
st = m.excite_status(); mot = m.motors(); dbg = m.debug()
check("sine running", st["state"] == "run" and st["session"] == 101, st)
check("rms ~ 50/sqrt2", abs(st["rms"] - 35.4) < 2.0, st["rms"])
check("motor 1 moving, others stopped", 1095 <= mot[0] <= 1205 and mot[1:4] == [1000] * 3, mot)
m.excite_stop(); time.sleep(0.1)
st = m.excite_status(); mot = m.motors()
check("stop -> ended/stop, motor 1000", st["state"] == "ended" and st["end"] == "stop" and mot[0] == 1000, (st["end"], mot[:4]))

# 2. keepalive timeout
m.excite_start(102, 0b0010, "none", 1100, duration=30, ramp=0, timeout=0.3)
time.sleep(0.15); mot_run = m.motors()
time.sleep(0.5)
st = m.excite_status(); mot = m.motors()
check("baseline ran on motor 2", mot_run[1] == 1100, mot_run[:4])
check("timeout -> ended/timeout, motor 1000", st["end"] == "timeout" and mot[1] == 1000, (st["end"], mot[:4]))

# 3. rejected parameters
for name, kw in [("base above cap", dict(base=1900, maxcmd=1900)),
                 ("f0 above 0.45*loop", dict(base=1150, waveform="sine", amp=10, f0=3700)),
                 ("no motor", dict(base=1150, mask=0)),
                 ("motor beyond count", dict(base=1150, mask=0b10000)),
                 ("amp above 500", dict(base=1150, waveform="sine", amp=600, f0=100))]:
    args = dict(session=103, mask=1, waveform="none", base=1150, duration=5)
    args.update(kw)
    try:
        m.excite_start(**args); rej = False
    except MspError:
        rej = True
    m.excite_stop()
    check("reject " + name, rej)

# 4. chirp ends by itself at f1
m.excite_start(104, 0b0100, "chirp", 1200, 40, 20, 1000, duration=2.0, ramp=0.2)
fmax = 0; t = time.time()
while time.time() - t < 3.5:
    m.excite_keepalive(104); st = m.excite_status()
    if st["state"] == "run": fmax = max(fmax, st["freq_hz"])
    if st["state"] == "ended": break
    time.sleep(0.05)
mot = m.motors()
check("chirp ended/done", st["state"] == "ended" and st["end"] == "done", st)
check("chirp reached ~f1", fmax > 800, fmax)
check("chirp motor stopped", mot[2] == 1000, mot[:4])

# 5. clipping reported (white sigma 100 on base 1050: lower half clipped at 1020)
m.excite_start(105, 0b1000, "white", 1050, 100, 300, duration=10, ramp=0, seed=7)
run_for(1.0, 105); st = m.excite_status(); m.excite_stop()
check("clipping reported", 20 < st["clipped_pct"] < 60, st["clipped_pct"])

# 6. handover without stop: session on motor 1, then motor 2 -> motor 1 stopped, motor 2 ramping
m.excite_start(106, 0b0001, "none", 1150, duration=10, ramp=0)
run_for(0.3, 106)
m.excite_start(107, 0b0010, "none", 1150, duration=10, ramp=0.5)
run_for(0.6, 107); mot = m.motors(); st = m.excite_status()
check("handover: m1 stopped, m2 running", mot[0] == 1000 and mot[1] == 1150 and st["session"] == 107, mot[:4])
m.excite_stop(); time.sleep(0.1)
check("all stopped", m.motors()[:4] == [1000] * 4, m.motors()[:4])
print("ALL PASS" if ok else "SOME FAILED")
m.close()
