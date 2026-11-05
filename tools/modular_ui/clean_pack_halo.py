#!/usr/bin/env python3
"""Remove the dark-grey disc/halo from generated character packs.

Reads a pack .c (RGB565 arrays), pushes near-black pixels to pure #000000 with a soft knee
and fades the outer rim of the circle to black, then rewrites the file in place.
Usage: clean_pack_halo.py pack.c [pack.c ...]
"""
import re
import sys
import numpy as np

KNEE_LO, KNEE_HI, RIM = 14.0, 46.0, 7.0


def clean(a565):
    size = a565.shape[0]
    r = ((a565 >> 11) & 31).astype(np.float32) * 255 / 31
    g = ((a565 >> 5) & 63).astype(np.float32) * 255 / 63
    b = (a565 & 31).astype(np.float32) * 255 / 31
    rgb = np.stack([r, g, b], -1)
    m = rgb.max(-1)
    t = np.clip((m - KNEE_LO) / (KNEE_HI - KNEE_LO), 0, 1)
    k = t * t * (3 - 2 * t)  # smoothstep
    yy, xx = np.mgrid[:size, :size]
    d = np.hypot(yy - (size - 1) / 2, xx - (size - 1) / 2)
    rim = np.clip((size / 2 - 1 - d) / RIM, 0, 1)
    rim = rim * rim * (3 - 2 * rim)
    rgb *= (k * rim)[..., None]
    r5 = np.rint(rgb[..., 0] * 31 / 255).astype(np.uint16)
    g6 = np.rint(rgb[..., 1] * 63 / 255).astype(np.uint16)
    b5 = np.rint(rgb[..., 2] * 31 / 255).astype(np.uint16)
    return (r5 << 11) | (g6 << 5) | b5


def process(path):
    s = open(path).read()

    def repl(m):
        data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(2)))
        n = int((len(data) // 2) ** 0.5)
        a = np.frombuffer(data, "<u2").reshape(n, n)
        out = clean(a).astype("<u2").tobytes()
        lines = ["  " + ",".join(f"0x{x:02x}" for x in out[i:i + 24]) + "," for i in range(0, len(out), 24)]
        return f"static const uint8_t {m.group(1)}_map[] = {{\n" + "\n".join(lines) + "\n};"

    s = re.sub(r"static const uint8_t (\w+)_map\[\] = \{(.*?)\};", repl, s, flags=re.S)
    open(path, "w").write(s)
    print("cleaned", path)


for p in sys.argv[1:]:
    process(p)
