#!/usr/bin/env python3
# Fetch the Vita's comparison captures (shot_N_gpu.ppm / shot_N_cpu.ppm) over FTP and count differing pixels.
#   python vita/tools/shotdiff.py [out_dir] [frame ...]
import os, sys, urllib.request
sys.stdout.reconfigure(encoding="utf-8")

IP = os.environ.get("VITA_IP", "192.168.0.33")
BASE = f"ftp://{IP}:1337/ux0:/data/restv/"


def ppm(data):
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][: w * h * 3]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    frames = sys.argv[2:] or ["600", "1500", "1800", "2100", "2400"]
    os.makedirs(out, exist_ok=True)
    bad = 0
    for n in frames:
        imgs = {}
        for k in ("gpu", "cpu"):
            name = f"shot_{n}_{k}.ppm"
            try:
                d = urllib.request.urlopen(BASE + name, timeout=10).read()
            except Exception as e:
                print(f"{n}: {name} missing ({e})")
                break
            open(os.path.join(out, name), "wb").write(d)
            imgs[k] = ppm(d)
        if len(imgs) < 2:
            bad += 1
            continue
        (w, h, a), (_, _, b) = imgs["gpu"], imgs["cpu"]
        diff = 0
        first = None
        for i in range(0, min(len(a), len(b)), 3):
            if a[i : i + 3] != b[i : i + 3]:
                diff += 1
                if first is None:
                    first = (i // 3 % w, i // 3 // w, tuple(a[i : i + 3]), tuple(b[i : i + 3]))
        print(f"{n}: {w}x{h} diff {diff}" + (f" first at {first}" if first else ""))
        bad += diff > 0
    sys.exit(1 if bad else 0)


main()
