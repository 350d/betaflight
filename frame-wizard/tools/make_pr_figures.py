#!/usr/bin/env python3
"""Figures for the frame wizard / filter autotune PR (betaflight#15851).

  make_pr_figures.py            -> img/filter_chain.png, img/notch_width.png, img/frame_map_vs_flight.png, numbers on stdout

Inputs: logs/hover3.bbl (5" hover on BF default filters) and the post-RPM gyro of flight 5 (scratch npy, see below).
"""
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import filter_sim as S          # noqa: E402
import hover_compare as H       # noqa: E402

IMG = os.path.join(HERE, "img")
os.makedirs(IMG, exist_ok=True)
SC = "/private/tmp/claude-501/-Volumes-2TB-GITHUB-betaflight/52046c22-e57a-4d7a-a799-fa6744cb388c/scratchpad"
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3, "figure.dpi": 120})
C_DEF, C_AT, C_RAW = "#7a7a7a", "#e8a33d", "#b9c4d6"


# ---------------- analytic delay of a full chain at the PID rate ----------------

def biquad_notch(f, f0, q, fs):
    b, a = S.notch_coef(f0, q, fs)
    z = np.exp(-1j * 2 * np.pi * f / fs)
    return (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)


def pt1(f, fc, fs):
    k = S.pt1_k(fc, fs)
    z = np.exp(-1j * 2 * np.pi * f / fs)
    return k / (1 - (1 - k) * z)


def chain_delay_ms(f, cfg, rot, fs=4000.0):
    """Phase delay at f (ms) of RPM notches (per motor, weighted), dynamic notches, a static notch and the LPFs."""
    h = np.ones_like(np.atleast_1d(f), dtype=complex)
    for r in rot:
        for k, w in enumerate(cfg["rpm_w"]):
            if w > 0:
                h *= 1 - w + w * biquad_notch(f, (k + 1) * r, cfg["rpm_q"], fs)
    for f0 in cfg["dyn_at"][:cfg["dyn"]]:
        h *= biquad_notch(f, f0, cfg["dyn_q"], fs)
    if cfg.get("notch"):
        f0, cut = cfg["notch"]
        h *= biquad_notch(f, f0, S.notch_q(f0, cut), fs)
    for fc in (cfg["lpf1"], cfg["lpf2"]):
        if fc:
            h *= pt1(f, fc, fs)
    return -np.unwrap(np.angle(h)) / (2 * np.pi * np.atleast_1d(f)) * 1000


def main():
    names, d, hdr = H.load(os.path.join(os.path.dirname(HERE), "logs/hover3.bbl"))
    c = lambda n: d[:, names.index(n)]
    t = c("time") * 1e-6
    t -= t[0]
    fs = 1 / np.median(np.diff(t))
    m = (t >= 10) & (t <= 66)
    gu = np.array([c(f"gyroUnfilt[{k}]")[m] * 0.1 for k in range(3)])
    rot = np.array([c(f"eRPM[{k}]")[m] * 100 / 7 / 60 for k in range(4)])
    thr = c("rcCommand[3]")[m] / 10000.0
    rot_mean = rot.mean(1)

    # BF defaults vs the autotune result on this hover (host replay of the firmware code: 95/75/90, q 500, dyn 1,
    # lpf1 off, lpf2 off, notch1 386/355)
    default = S.Chain("BF defaults", rpm_w=(1, 1, 1), rpm_q=5.0, dyn_count=3, dyn_q=3.0, glpf1=(250, 500), glpf2=500)
    tuned = S.Chain("autotune", rpm_w=(0.95, 0.75, 0.90), rpm_q=5.0, dyn_count=1, dyn_q=3.0, static=((386, 355),),
                    glpf1=None, glpf2=None)
    res = {}
    for ch in (default, tuned):
        x, dt = S.run(ch, gu, rot, thr, fs)
        F, P = signal.welch(x - x.mean(1, keepdims=True), fs=fs, nperseg=1024, axis=1)
        _, Pd = signal.welch(dt[:2], fs=fs, nperseg=1024, axis=1)
        res[ch.name] = (F, P.sum(0), np.sqrt(Pd[:, F > 20].sum() * (F[1] - F[0])))
    F, Pu = signal.welch(gu - gu.mean(1, keepdims=True), fs=fs, nperseg=1024, axis=1)
    Pu = Pu.sum(0)

    def band(P, lo, hi):
        return np.sqrt(P[(F >= lo) & (F < hi)].sum() * (F[1] - F[0]))

    cfg_def = dict(rpm_w=(1, 1, 1), rpm_q=5.0, dyn=3, dyn_q=3.0, dyn_at=(150, 300, 450), lpf1=300, lpf2=500)
    cfg_at = dict(rpm_w=(0.95, 0.75, 0.90), rpm_q=5.0, dyn=1, dyn_q=3.0, dyn_at=(382,), notch=(386, 355), lpf1=0, lpf2=0)
    fq = np.array([30.0, 50.0, 100.0])
    dl_def, dl_at = chain_delay_ms(fq, cfg_def, rot_mean), chain_delay_ms(fq, cfg_at, rot_mean)
    print("hover3 gyro rms (deg/s), 3 axes: band  unfiltered  defaults  autotune")
    for lo, hi in ((20, 100), (100, 200), (200, 350), (350, 450), (450, 600), (600, 978)):
        print(f"  {lo:3d}-{hi:3d} Hz  {band(Pu, lo, hi):7.2f}  {band(res['BF defaults'][1], lo, hi):7.2f}  {band(res['autotune'][1], lo, hi):7.2f}")
    print(f"  D-term input rms (deg/s^2): defaults {res['BF defaults'][2]:.0f}, autotune {res['autotune'][2]:.0f}")
    print("chain delay (ms) at 30/50/100 Hz: defaults", np.round(dl_def, 3), "autotune", np.round(dl_at, 3))

    fig, ax = plt.subplots(figsize=(10, 4.6))
    ax.semilogy(F, Pu, color=C_RAW, lw=1, label="raw gyro")
    ax.semilogy(F, res["BF defaults"][1], color=C_DEF, lw=1.4, label="after BF default filters")
    ax.semilogy(F, res["autotune"][1], color=C_AT, lw=1.6, label="after the autotune's filters")
    ax.set_xlim(20, 950)
    ax.set_xlabel("Hz")
    ax.set_ylabel("gyro PSD, 3 axes ((deg/s)$^2$/Hz)")
    ax.set_title('5" hover (hover3): the same raw gyro through both filter chains')
    txt = (f"delay at 50 / 100 Hz: defaults {dl_def[1]:.2f} / {dl_def[2]:.2f} ms, autotune {dl_at[1]:.2f} / {dl_at[2]:.2f} ms\n"
           f"D-term input rms: defaults {res['BF defaults'][2]:.0f}, autotune {res['autotune'][2]:.0f} deg/s$^2$")
    ax.text(0.99, 0.97, txt, transform=ax.transAxes, ha="right", va="top", fontsize=9,
            bbox=dict(boxstyle="round", fc="white", ec="#cccccc"))
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "filter_chain.png"))

    # notch width: share of the bump's excess taken vs q, with 3 % drift, and the delay it costs
    post = S.run(S.Chain("rpm", rpm_w=(1, 0.8, 1), rpm_q=3.0, dyn_count=0, glpf1=None, glpf2=None, dlpf1=None, dlpf2=None),
                 gu, rot, thr, fs)[0]
    F2, P2 = signal.welch(post - post.mean(1, keepdims=True), fs=fs, nperseg=1024, axis=1)
    P2 = P2.sum(0)
    k = np.argmax(np.where((F2 > 350) & (F2 < 410), P2, 0))
    f0 = F2[k]
    side = lambda lo, hi: np.median(P2[(F2 > lo) & (F2 < hi)])
    ref = max(side(f0 * 0.80, f0 * 0.90), side(f0 * 1.10, f0 * 1.20))
    ex = np.clip(P2 - ref, 0, None) * ((F2 > 300) & (F2 < 470))

    def h2(f, fc, q):
        u = f / fc
        a = (1 - u * u) ** 2
        return a / (a + (u / q) ** 2)
    qs = np.array([1.5, 2, 2.5, 3, 4, 5, 6, 8, 12])
    take = [np.mean([(ex * (1 - h2(F2, f0 * s, q))).sum() for s in (0.97, 1.0, 1.03)]) / ex.sum() for q in qs]
    dly = [chain_delay_ms(np.array([100.0]), dict(rpm_w=(), rpm_q=1, dyn=0, dyn_q=1, dyn_at=(), notch=(f0, f0 * (np.sqrt(1 + 4 * q * q) - 1) / (2 * q)), lpf1=0, lpf2=0), [])[0] for q in qs]
    fig, ax = plt.subplots(figsize=(7.5, 4))
    ax.plot(qs, np.array(take) * 100, "o-", color=C_AT, label="share of the bump's excess taken (bump 3 % off)")
    ax.axhline(85, color="#999999", ls="--", lw=1)
    ax.text(12, 86, "85 % target", ha="right", fontsize=8, color="#666666")
    ax.set_xscale("log")
    ax.set_xticks(qs)
    ax.set_xticklabels([f"{q:g}" for q in qs])
    ax.set_xlabel("static notch Q")
    ax.set_ylabel("% of the excess taken")
    ax2 = ax.twinx()
    ax2.plot(qs, dly, "s--", color=C_DEF, label="delay at 100 Hz")
    ax2.set_ylabel("delay at 100 Hz (ms)")
    ax2.grid(False)
    ax.set_title(f"Static notch width on the {f0:.0f} Hz hover bump: narrowest Q that takes 85 %")
    h1, l1 = ax.get_legend_handles_labels()
    h2_, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2_, l1 + l2, loc="lower left", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "notch_width.png"))
    print("notch width: f0", round(f0, 1), "q", list(qs), "take %", np.round(np.array(take) * 100, 1), "delay ms", np.round(dly, 3))

    # frame map vs the flight spectrum (flight 5, after the RPM filter)
    fl = np.load(os.path.join(SC, "f5_post.npy"), allow_pickle=True).item()
    F5, P5 = signal.welch(fl["post"] - fl["post"].mean(1, keepdims=True), fs=fl["fs"], nperseg=1024, axis=1)
    P5 = P5.sum(0)
    fmap = [(56.7, "c"), (71.8, "m"), (75.3, "t"), (80.9, "m"), (101.5, "c"), (129.9, "tm"), (170.2, "c"), (208.2, "tm"),
            (238.0, "t"), (264.4, "m"), (321.6, "t"), (333.4, "m"), (367.6, "m"), (438.6, "m"), (451.8, "c"), (515.5, "m")]
    fig, ax = plt.subplots(figsize=(10, 4.4))
    ax.semilogy(F5, P5, color="#4a6fa5", lw=1.3, label="hover gyro after the RPM filter (flight 5)")
    for hz, src in fmap:
        col = "#9a9a9a" if src == "c" else ("#2e8b57" if "t" in src and "m" in src else ("#3a7bd5" if "t" in src else "#c0504d"))
        ax.axvline(hz, color=col, lw=1, ls=":" if src == "c" else "-", alpha=0.8)
    ax.axvspan(370, 395, color=C_AT, alpha=0.25, label="bump the autotune notches (383/360 Hz)")
    for lab, col in (("tap mode", "#3a7bd5"), ("motor sweep mode", "#c0504d"), ("tap + motor", "#2e8b57"), ("USB cable (bench)", "#9a9a9a")):
        ax.plot([], [], color=col, label=lab)
    ax.set_xlim(40, 700)
    ax.set_xlabel("Hz")
    ax.set_ylabel("PSD, 3 axes")
    ax.set_title("Frame map from the wizard vs the hover spectrum")
    ax.legend(fontsize=8, loc="upper right", ncol=2)
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "frame_map_vs_flight.png"))
    print("figures in", IMG)


if __name__ == "__main__":
    main()
