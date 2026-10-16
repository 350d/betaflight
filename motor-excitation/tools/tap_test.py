#!/usr/bin/env python3
"""Tap (impact) test: motors stay stopped, the FC logs a RECORD session; voice prompts tell which arm (by motor
number) to tap. Markers in the log: arm*16 + 1 = prompt, arm*16 + 2 = tap window, 0 = rest.
Battery may be disconnected (gyro runs from USB). Blackbox: device SPIFLASH, mode MOTOR_TEST, sample rate 1/1.

  tap_test.py PORT [--taps 5] [--window 8] [--arms 1,2,3,4] [--name tap1]
"""
import argparse
import subprocess
import time

from msp import Msp

ARM = {1: "задний правый", 2: "передний правый", 3: "задний левый", 4: "передний левый"}   # Betaflight quad-X
# extra tap points: marker code = point * 16 + 2, prompt text
POINTS = {5: "батарея", 6: "дно рамы под стеком"}


def say(text):
    return subprocess.Popen(["say", "-v", "Milena", text])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--taps", type=int, default=5)
    ap.add_argument("--window", type=float, default=8.0, help="seconds per arm for the taps")
    ap.add_argument("--arms", default="5,6,1,2,3,4", help="motor numbers 1-4 and/or points 5 = battery, 6 = frame bottom under the stack")
    ap.add_argument("--session", type=int, default=950)
    a = ap.parse_args()
    arms = [int(x) for x in a.arms.split(",")]
    total = 2 + len(arms) * (3.5 + a.window + 1.5) + 2

    m = Msp(a.port)
    say("Тест ударом. Начинаем через пять секунд.").wait()
    time.sleep(3.0)
    m.excite_start(a.session, 0, "record", 1000, duration=total, ramp=0, maxcmd=1500, timeout=1.0)
    print(f"session {a.session}: RECORD {total:.0f} s, arms {arms}, {a.taps} taps each")
    t_end = time.time() + total

    def hold(marker, seconds):
        t = time.time() + seconds
        while time.time() < t:
            m.excite_mark(a.session, marker)
            time.sleep(0.2)

    try:
        hold(0, 2.0)   # quiet baseline
        for arm in arms:
            if arm in POINTS:
                print(f">>> point {arm} ({POINTS[arm]}): {a.taps} taps")
                p = say(f"{POINTS[arm]}. {a.taps} лёгких ударов пальцем.")
            else:
                print(f">>> motor {arm} ({ARM[arm]}): {a.taps} taps")
                p = say(f"Мотор {arm}, {ARM[arm]}. {a.taps} ударов.")
            hold(arm * 16 + 1, 3.5)
            p.wait()
            say("Бей.")
            hold(arm * 16 + 2, a.window)
            say("Стоп.")
            hold(0, 1.5)
        hold(0, max(0.0, min(2.0, t_end - time.time() - 0.3)))
        say("Готово.").wait()
    except KeyboardInterrupt:
        print("interrupted")
    finally:
        m.excite_stop()
        print("final status:", m.excite_status())
        m.close()


if __name__ == "__main__":
    main()
