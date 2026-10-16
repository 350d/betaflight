#!/usr/bin/env python3
"""Bench excitation session on the FC (USE_MOTOR_EXCITATION firmware), no props.

The FC generates the signal at PID rate; this script only starts the session, sends a keepalive,
logs status + telemetry, and stops the motors on exit (Ctrl-C, error, end). If this script dies the
FC stops the motors by itself after --timeout.

  excite.py PORT --motor 1 --base 1150 --duration 20                                  # baseline
  excite.py PORT --motor 1 --base 1150 --wave sine  --amp 50 --f0 201 --duration 60
  excite.py PORT --motor 1 --base 1150 --wave white --amp 35 --f0 300 --seed 7 --duration 60
  excite.py PORT --motor 1 --base 1200 --wave chirp --amp 40 --f0 20 --f1 1000 --duration 30
  excite.py PORT --motor 1 --base 1200 --wave square --amp 60 --f0 5 --duration 10
  excite.py PORT --status                                                              # read status only
"""
import argparse
import csv
import os
import random
import sys
import time

from msp import Msp, MspError


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--motor", default="1", help="motor number(s) 1..8, comma separated")
    ap.add_argument("--wave", choices=["none", "sine", "white", "chirp", "square"], default="none")
    ap.add_argument("--base", type=int, default=1150, help="base command, 1000..2000 units")
    ap.add_argument("--amp", type=float, default=0, help="sine/chirp/square: peak; white: sigma (units)")
    ap.add_argument("--f0", type=float, default=0, help="Hz: sine/square frequency, white lowpass, chirp start")
    ap.add_argument("--f1", type=float, default=0, help="chirp end, Hz")
    ap.add_argument("--duration", type=float, default=10, help="signal seconds after the ramp")
    ap.add_argument("--ramp", type=float, default=2.0, help="base ramp-in seconds")
    ap.add_argument("--max", type=int, default=1500, help="command clamp, units (firmware cap 1800)")
    ap.add_argument("--timeout", type=float, default=1.0, help="FC stops the motors after this without keepalive, s")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--session", type=int, default=None, help="session id (default random)")
    ap.add_argument("--max-amps", type=float, default=3.0, help="stop if FC current exceeds this (A)")
    ap.add_argument("--min-vbat", type=float, default=0.0, help="stop if battery voltage drops below this (V), 0 = off")
    ap.add_argument("--sample", type=float, default=0.25, help="status/telemetry period, s")
    ap.add_argument("--log", default=None)
    ap.add_argument("--status", action="store_true", help="print the excitation status and exit")
    ap.add_argument("--bbl", default=None, help="pull the blackbox stream (blackbox_device = MSP) into this file")
    ap.add_argument("--bb-inflight", type=int, default=2, help="stream requests in flight")
    a = ap.parse_args()

    m = Msp(a.port)
    if a.status:
        print(m.excite_status())
        return

    motors = [int(x) for x in a.motor.split(",")]
    mask = sum(1 << (i - 1) for i in motors)
    session = a.session if a.session is not None else random.randint(1, 30000)

    f = w = None
    if a.log:
        os.makedirs(os.path.dirname(a.log) or ".", exist_ok=True)
        f = open(a.log, "w", newline="")
        w = csv.writer(f)
        w.writerow(["unix", "t", "session", "state", "end", "freq_hz", "sample", "rms", "clipped_pct",
                    "vbat", "amps", "rpm", "invalid_pct", "esc_temp"])
        # run metadata in a sidecar file, same base name
        with open(os.path.splitext(a.log)[0] + "_meta.txt", "w") as mf:
            mf.write(" ".join(sys.argv) + "\n")
            mf.write(f"session={session} mask={mask} wave={a.wave} base={a.base} amp={a.amp} f0={a.f0} f1={a.f1} "
                     f"duration={a.duration} ramp={a.ramp} max={a.max} seed={a.seed}\n")

    bb = open(a.bbl, "wb") if a.bbl else None
    bb_next = 0
    bb_drop0 = None
    if bb:   # announce the reader before the start so the FC opens the log when the motors start
        for _ in range(3):
            m.bb_stream_read(16)
            time.sleep(0.05)
    print(f"session {session}: motors {motors} {a.wave} base {a.base} amp {a.amp} f0 {a.f0} f1 {a.f1} "
          f"for {a.ramp}+{a.duration} s. Ctrl-C stops.")
    t0 = time.time()
    try:
        m.excite_start(session, mask, a.wave, a.base, a.amp, a.f0, a.f1, a.duration, a.ramp, a.max, a.timeout, a.seed)
        next_ka = next_sample = t0
        last_print = 0
        idx = motors[0] - 1
        ended_at = None
        while True:
            now = time.time()
            if bb:
                for r in m.bb_stream_read_n(a.bb_inflight):
                    off, dropped, data = r
                    if bb_drop0 is None:
                        bb_drop0 = dropped
                    if off != bb_next and bb_next:
                        print(f"  stream gap: expected {bb_next}, got {off}")
                    bb.write(data)
                    bb_next = off + len(data)
                    bb_dropped = dropped - bb_drop0
                if ended_at and now - ended_at > 6.0:   # blackbox keeps logging 5 s after motors stop
                    break
            if now >= next_ka:
                m.excite_keepalive(session)
                next_ka = now + a.timeout / 4
            if now >= next_sample:
                next_sample = now + a.sample
                st = m.excite_status()
                an = m.analog() or {}
                tel = m.motor_telemetry() or []
                tm = tel[idx] if len(tel) > idx else {}
                if st is None:
                    continue
                if w:
                    w.writerow([f"{now:.3f}", f"{now - t0:.3f}", st["session"], st["state"], st["end"],
                                st["freq_hz"], st["sample"], st["rms"], f"{st['clipped_pct']:.2f}",
                                an.get("vbat"), an.get("amps"), tm.get("rpm"), tm.get("invalid_pct"), tm.get("temp")])
                if now - last_print >= 1.0:
                    last_print = now
                    print(f"  t={now - t0:6.1f}s {st['state']:5s} f={st['freq_hz']:7.1f}Hz rms={st['rms']:5.1f} "
                          f"clip={st['clipped_pct']:4.1f}% vbat={an.get('vbat')} A={an.get('amps')} rpm={tm.get('rpm')}")
                if st["session"] == session and st["state"] == "ended" and not ended_at:
                    print(f"ended: {st['end']}")
                    if not bb:
                        break
                    ended_at = now
                if an.get("amps", 0) > a.max_amps:
                    print(f"STOP: current {an['amps']} A > {a.max_amps}")
                    break
                vb = an.get("vbat", 0)
                if a.min_vbat and 1.0 < vb < a.min_vbat:
                    print(f"STOP: vbat {vb} < {a.min_vbat}")
                    break
            if not bb:
                time.sleep(0.005)
    except KeyboardInterrupt:
        print("interrupted")
    except MspError as e:
        print(f"FC rejected the session: {e}")
    finally:
        m.excite_stop()
        st = m.excite_status()
        print("final status:", st)
        if f:
            f.close()
        if bb:
            bb.close()
            print(f"blackbox stream: {bb_next} bytes, dropped by FC {locals().get('bb_dropped', 0)}")
        m.close()


if __name__ == "__main__":
    main()
