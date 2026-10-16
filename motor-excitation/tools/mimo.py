#!/usr/bin/env python3
"""Multi-input transfer functions: all motors driven at once with independent noise; for every output (gyro axis)
solve csd(u_i, y) = sum_j H_j csd(u_i, u_j) per frequency. Returns H (f, inputs) and the multiple coherence."""
import numpy as np
from scipy import signal


def mimo_transfer(U, y, fs, nperseg=4096):
    """U: (n, k) inputs, y: (n,) output. Returns f, H (len f, k), multiple coherence (len f)."""
    k = U.shape[1]
    f, _ = signal.welch(y, fs, nperseg=nperseg)
    A = np.zeros((len(f), k, k), complex)
    b = np.zeros((len(f), k), complex)
    for i in range(k):
        for j in range(k):
            A[:, i, j] = signal.csd(U[:, i], U[:, j], fs, nperseg=nperseg)[1]
        b[:, i] = signal.csd(U[:, i], y, fs, nperseg=nperseg)[1]
    _, syy = signal.welch(y, fs, nperseg=nperseg)
    H = np.zeros((len(f), k), complex)
    coh = np.zeros(len(f))
    for m in range(len(f)):
        try:
            H[m] = np.linalg.solve(A[m], b[m])
        except np.linalg.LinAlgError:
            H[m] = np.nan
        coh[m] = np.real(np.conj(b[m]) @ H[m]) / syy[m] if syy[m] > 0 else 0
    return f, H, np.clip(coh, 0, 1)


def selftest():
    rng = np.random.default_rng(7)
    fs, n = 3900.0, int(3900 * 15)
    U = rng.standard_normal((n, 4)) * 30
    modes = {0: (209.0, 40), 1: (268.0, 30), 2: (335.0, 25), 3: (434.0, 50)}   # input i excites its own mode
    y = np.zeros(n)
    for i, (f0, q) in modes.items():
        b, a = signal.iirpeak(f0, q, fs)
        y += (0.2 + 0.1 * i) * signal.lfilter(b, a, U[:, i])
    y += 0.1 * signal.lfilter(*signal.iirpeak(209.0, 40, fs), U.sum(axis=1))   # a shared craft mode
    y += rng.standard_normal(n) * 1.0
    f, H, coh = mimo_transfer(U, y, fs, nperseg=4096)
    ok = True
    for i, (f0, q) in modes.items():
        mag = np.abs(H[:, i]); band = f > 50
        fp = f[band][np.argmax(mag[band])]
        g = mag[np.argmin(abs(f - f0))]
        expect = 0.2 + 0.1 * i + (0.1 if i == 0 else 0.0)
        print(f"input {i + 1}: peak {fp:.1f} Hz (true {f0}), gain {g:.3f} (true {expect:.3f})")
        ok &= abs(fp - f0) < 4 and abs(g - expect) / expect < 0.15
    print("multiple coherence at 268 Hz", round(float(coh[np.argmin(abs(f - 268))]), 2))
    print("SELFTEST", "PASS" if ok else "FAIL")
    return ok


if __name__ == "__main__":
    selftest()
