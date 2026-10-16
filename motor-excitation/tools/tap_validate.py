#!/usr/bin/env python3
"""Ground truth check of the tap classifier in random order: the voice names one point at a time, then a series of
--per taps on it (points shuffled).
Markers: point * 16 + 3 while that point is requested. Motors stay stopped (RECORD session).

  tap_validate.py PORT [--per 5] [--seed 1]      record
  tap_validate.py --report logs/x.bbl             confusion matrix
"""
import argparse
import random
import subprocess
import sys
import time

import numpy as np

NAMES = {1: "Мотор один", 2: "Мотор два", 3: "Мотор три", 4: "Мотор четыре", 5: "Батарея", 6: "Дно"}
LABEL = {0: "unsure", 1: "arm 1", 2: "arm 2", 3: "arm 3", 4: "arm 4", 5: "battery", 6: "bottom"}


def record(a):
    from msp import Msp
    seq = list(range(1, 7))
    random.Random(a.seed).shuffle(seq)
    m = Msp(a.port)
    say = lambda text: subprocess.Popen(["say", "-v", "Milena", "-r", "220", text])

    def hold(marker, seconds, speech=None):
        t = time.time() + seconds
        while time.time() < t or (speech is not None and speech.poll() is None):
            m.excite_mark(950, marker)
            time.sleep(0.1)

    # reset the Bluejay idle beacon timer (it starts beeping after 10 min at zero throttle) with a short spin
    say("Моторы коротко крутнутся.").wait()
    m.excite_start(949, 0b1111, "none", 1060, duration=0.5, ramp=0, maxcmd=1100, timeout=1.0)
    time.sleep(0.8)
    m.excite_stop()
    time.sleep(0.5)
    window = 1.4 * a.per + 1.0
    total = 15 + len(seq) * (window + 3.0)
    sp = say(f"Проверка разметки. Шесть точек в случайном порядке, по {a.per} ударов. Батарея сверху, дно снизу, лучи сверху.")
    sp.wait()
    m.excite_start(950, 0, "record", 1000, duration=total, ramp=0, maxcmd=1500, timeout=1.0)
    try:
        hold(0, 1.5)
        for k, p in enumerate(seq):
            print(f"{k + 1}/{len(seq)} {NAMES[p]}: {a.per} taps")
            sp = say(f"{NAMES[p]}, {a.per}.")
            hold(0, 0.5, speech=sp)
            hold(p * 16 + 3, window)
            say("Стоп.")
            hold(0, 1.2)
        say("Готово.").wait()
    finally:
        m.excite_stop()
        m.close()


def report(path, per=5):
    import bbl_report as br
    import tap_report as tr
    import tap_classify as tc
    names, d, hdr = br.load(path)
    col = lambda n: br.col(names, d, n)
    gs = 0.1 if int(hdr.get("blackbox_high_resolution", 0) or 0) else 1.0
    t = col("time") * 1e-6; o = np.argsort(t); t = t[o]
    g = np.column_stack([col(f"gyroUnfilt[{k}]")[o] * gs for k in range(3)])
    acc = np.column_stack([col(f"accSmooth[{k}]")[o] for k in range(3)]) / float(hdr.get("acc_1G", 2048) or 2048)
    st = col("debug[3]")[o].astype(int) >> 4; mk = col("debug[2]")[o].astype(int)
    fs = (len(t) - 1) / (t[-1] - t[0])
    rec = np.flatnonzero(st == 2)
    a, b = rec[0], rec[-1]
    onsets = [a + i for i in tr.find_taps(g[a:b], fs)[0]]
    truth = [(mk[i] >> 4) if (mk[i] & 0xF) == 3 else -1 for i in onsets]
    labels, thr = tc.label_taps(t, g, acc, onsets, fs)
    windows = int(np.sum(np.diff(((mk[a:b] & 0xF) == 3).astype(int)) == 1))
    print(f"{path}: {windows} prompts, {len(onsets)} taps found, arm/centre split {thr:.3f} deg")
    pts = [1, 2, 3, 4, 5, 6]
    cm = np.zeros((7, 7), int)
    outside = 0
    for tv, lv in zip(truth, labels):
        if tv < 0:
            outside += 1
            continue
        cm[tv, lv] += 1
    print("prompted \\ classified: " + " ".join(f"{LABEL[c]:>7s}" for c in [1, 2, 3, 4, 5, 6, 0]))
    for r in pts:
        print(f"  {LABEL[r]:>8s}            " + " ".join(f"{cm[r, c]:7d}" for c in [1, 2, 3, 4, 5, 6, 0]))
    n = cm[1:, :].sum(); ok = sum(cm[p, p] for p in pts); uns = cm[1:, 0].sum()
    print(f"correct {ok}/{n}, unsure {uns}, wrong {n - ok - uns}; taps outside prompt windows {outside}")
    print("taps found per series (asked " + str(per) + "): " + ", ".join(f"{LABEL[q]} {cm[q].sum()}" for q in pts))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--per", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--report", default=None)
    a = ap.parse_args()
    if a.report:
        report(a.report, a.per)
    else:
        record(a)
