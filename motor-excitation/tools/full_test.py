#!/usr/bin/env python3
"""Quick full bench test, no props, battery connected, craft held by the battery.
Phase 1: taps, 5 each: battery, frame bottom, arms 1-4 (RECORD session 950, voice prompts by motor number).
Phase 2: all four motors at once, independent noise per motor, base 1150 then 1300, 15 s each (sessions 1001/1002).
Blackbox: SPIFLASH, MOTOR_TEST, 1/1. Then: bbl_download.py and full_report.py.

  full_test.py PORT [--taps 5] [--window 8] [--spin 15] [--amp 30]
"""
import argparse
import subprocess
import time

from msp import Msp, MspError

ARM = {1: "задний правый", 2: "передний правый", 3: "задний левый", 4: "передний левый"}
POINTS = {5: "батарея", 6: "дно рамы под стеком"}


def say(text, wait=False):
    p = subprocess.Popen(["say", "-v", "Milena", text])
    if wait:
        p.wait()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--taps", type=int, default=5)
    ap.add_argument("--window", type=float, default=8.0)
    ap.add_argument("--spin", type=float, default=15.0, help="seconds per base speed in phase 2")
    ap.add_argument("--amp", type=float, default=30.0, help="noise sigma per motor, command units")
    ap.add_argument("--bases", default="1150,1300")
    ap.add_argument("--max-amps", type=float, default=5.0)
    ap.add_argument("--free", type=float, default=0.0, help="free tap window, seconds: any order, labelled from the gyro")
    ap.add_argument("--min-vbat", type=float, default=13.2)
    a = ap.parse_args()
    points = [5, 6, 1, 2, 3, 4]
    bases = [int(x) for x in a.bases.split(",")]

    m = Msp(a.port)
    an = m.analog() or {}
    if an.get("vbat", 0) < 6:
        say("Подключи батарею.", wait=True)
        raise SystemExit(f"battery not connected: {an}")
    say("Полный тест. Держи дрон за батарею. Начинаем через пять секунд.", wait=True)
    time.sleep(3.0)

    # ---- phase 1: taps ----
    total = (2 + 20 + a.free + 2) if a.free else (2 + len(points) * (6 + a.window + 1.5) + 1)   # speech margin
    m.excite_start(950, 0, "record", 1000, duration=total, ramp=0, maxcmd=1500, timeout=1.0)

    def hold(session, marker, seconds, speech=None):
        """keep the session alive with a marker for `seconds`, and at least until `speech` has finished"""
        t = time.time() + seconds
        while time.time() < t or (speech is not None and speech.poll() is None):
            m.excite_mark(session, marker)
            time.sleep(0.2)

    try:
        hold(950, 0, 2.0)
        if a.free:
            sp = say(f"Свободный режим. Бей по {a.taps} раз: по батарее сверху, по дну рамы снизу и по каждому лучу сверху, "
                     f"в любом порядке. {int(a.free)} секунд.")
            hold(950, 0, 1.0, speech=sp)
            say("Бей.")
            hold(950, 7 * 16 + 2, a.free)
            say("Стоп.")
            hold(950, 0, 1.5)
            points = []
        for p in points:
            text = f"{POINTS[p]}. {a.taps} лёгких ударов." if p in POINTS else f"Мотор {p}, {ARM[p]}. {a.taps} ударов."
            print(">>>", text)
            sp = say(text)
            hold(950, p * 16 + 1, 1.0, speech=sp)
            say("Бей.")
            hold(950, p * 16 + 2, a.window)
            say("Стоп.")
            hold(950, 0, 1.5)
        m.excite_stop()

        # ---- phase 2: all motors, independent noise, two base speeds ----
        say("Фаза два. Все моторы крутятся. Держи крепко, пальцы подальше от моторов.", wait=True)
        time.sleep(1.0)
        for k, base in enumerate(bases):
            session = 1001 + k
            m.excite_start(session, 0b1111, "white", base, a.amp, 800, duration=a.spin, ramp=1.5,
                           maxcmd=base + 200, timeout=1.0, seed=11 + k, independent=True)
            print(f">>> phase 2: base {base}, {a.spin} s")
            t = time.time()
            while time.time() - t < a.spin + 1.5:
                m.excite_keepalive(session)
                an = m.analog() or {}
                if an.get("amps", 0) > a.max_amps or (an.get("vbat", 99) < a.min_vbat):
                    print("STOP: limits", an)
                    raise KeyboardInterrupt
                time.sleep(0.2)
        m.excite_stop()
        say("Готово. Можно положить.", wait=True)
    except (KeyboardInterrupt, MspError) as e:
        print("stopped:", e)
    finally:
        m.excite_stop()
        print("final status:", m.excite_status())
        m.close()


if __name__ == "__main__":
    main()
