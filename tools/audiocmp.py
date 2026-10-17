#!/usr/bin/env python3
# Compare two audio dumps (harness --wav, raw 16-bit stereo).
#   python3 tools/audiocmp.py ref.raw test.raw [window_sec=5]
# Per window: best SNR over lag 0..32 samples, 20ms RMS envelope correlation, level ratio (test/ref), zero-crossing rate ratio (pitch proxy).
# The batched sound engine (--scspbatch) applies 68K register writes up to one batch late, so waveform SNR can be low.
# Envelope correlation, level ratio and zero-crossing ratio near 1 count as the same sound.
import math
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    n = len(d) // 4
    v = struct.unpack("<%dh" % (n * 2), d[: n * 4])
    return [(v[2 * i] + v[2 * i + 1]) * 0.5 for i in range(n)]  # mono


def snr(a, b, lag):
    sig = err = 0.0
    m = min(len(a), len(b) - lag)
    for i in range(0, m, 2):
        x, y = a[i], b[i + lag]
        sig += x * x
        err += (x - y) * (x - y)
    if err == 0:
        return 99.0
    if sig == 0:
        return -99.0
    return 10 * math.log10(sig / err)


def rms_env(a, w=882):
    return [math.sqrt(sum(x * x for x in a[i : i + w]) / w) for i in range(0, len(a) - w + 1, w)]


def corr(x, y):
    n = min(len(x), len(y))
    if n < 2:
        return 1.0
    mx, my = sum(x[:n]) / n, sum(y[:n]) / n
    sxy = sum((x[i] - mx) * (y[i] - my) for i in range(n))
    sxx = sum((x[i] - mx) ** 2 for i in range(n))
    syy = sum((y[i] - my) ** 2 for i in range(n))
    if sxx == 0 or syy == 0:
        return 1.0 if sxx == syy else 0.0
    return sxy / math.sqrt(sxx * syy)


def zcr(a):
    return sum(1 for i in range(1, len(a)) if (a[i - 1] < 0) != (a[i] < 0)) / max(1, len(a))


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    seg = int(float(sys.argv[3]) * 44100) if len(sys.argv) > 3 else 44100 * 5
    n = min(len(a), len(b))
    print("samples", n)
    worst_c, worst_lvl = 1.0, 1.0
    for start in range(0, n - seg + 1, seg):
        sa, sb = a[start : start + seg], b[start : start + seg]
        ra, rb = rms_env(sa), rms_env(sb)
        la, lb = sum(ra) / len(ra), sum(rb) / len(rb)
        if la < 1 and lb < 1:
            continue
        best = max(snr(sa, sb, lag) for lag in range(0, 33, 4))
        c = corr(ra, rb)
        lvl = lb / la if la else 0
        z = zcr(sb) / zcr(sa) if zcr(sa) else 0
        worst_c, worst_lvl = min(worst_c, c), min(worst_lvl, lvl if lvl <= 1 else 1 / lvl)
        print("%6.1fs  SNR %6.1f dB  env-corr %.3f  level %.3f  zcr %.3f" % (start / 44100, best, c, lvl, z))
    print("worst env-corr %.3f  worst level ratio %.3f" % (worst_c, worst_lvl))


main()
