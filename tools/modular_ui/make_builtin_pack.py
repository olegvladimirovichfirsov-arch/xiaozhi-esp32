#!/usr/bin/env python3
"""Cut character circles out of a sheet image and emit an LVGL RGB565 C file.

Usage: make_builtin_pack.py SHEET.png ROW_INDEX PACK_ID OUT.c [SIZE]
Circles are found as large dark connected blobs; ROW_INDEX is 0-based.
States per row, left to right: joy, surprise, thoughtful, sad, sleep, greeting.
"""
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

STATES = ["joy", "surprise", "thoughtful", "sad", "sleep", "greeting"]


def find_circles(img):
    a = np.array(img.convert("RGB")).astype(int)
    dark = a.sum(axis=2) < 90
    lab, n = ndimage.label(dark)
    boxes = []
    for i, sl in enumerate(ndimage.find_objects(lab), start=1):
        h = sl[0].stop - sl[0].start
        w = sl[1].stop - sl[1].start
        if 150 < w < 330 and 150 < h < 330 and abs(w - h) < 25:
            boxes.append((sl[0].start, sl[1].start, h, w))
    boxes.sort()
    rows, cur = [], []
    for b in boxes:
        if cur and abs(b[0] - cur[0][0]) > 40:
            rows.append(sorted(cur, key=lambda t: t[1]))
            cur = []
        cur.append(b)
    if cur:
        rows.append(sorted(cur, key=lambda t: t[1]))
    return rows


def to_rgb565_bytes(img):
    a = np.array(img.convert("RGB"))
    r = (a[..., 0] >> 3).astype(np.uint16)
    g = (a[..., 1] >> 2).astype(np.uint16)
    b = (a[..., 2] >> 3).astype(np.uint16)
    v = (r << 11) | (g << 5) | b
    return v.astype("<u2").tobytes()


def main():
    sheet, row, pack, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    size = int(sys.argv[5]) if len(sys.argv) > 5 else 220
    img = Image.open(sheet)
    rows = find_circles(img)
    circles = rows[row]
    assert len(circles) == len(STATES), (len(circles), [len(r) for r in rows])
    lines = ['#include "lvgl.h"', ""]
    for state, (y, x, h, w) in zip(STATES, circles):
        c = img.convert("RGB").crop((x, y, x + w, y + h)).resize((size, size), Image.LANCZOS)
        yy, xx = np.ogrid[:size, :size]
        mask = (yy - size / 2 + 0.5) ** 2 + (xx - size / 2 + 0.5) ** 2 <= (size / 2) ** 2
        arr = np.array(c)
        arr[~mask] = 0
        data = to_rgb565_bytes(Image.fromarray(arr))
        name = f"{pack}_{state}"
        lines.append(f"static const uint8_t {name}_map[] = {{")
        for i in range(0, len(data), 24):
            lines.append("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append(f"const lv_image_dsc_t {name} = {{")
        lines.append("  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,")
        lines.append(f"              .flags = 0, .w = {size}, .h = {size}, .stride = {size * 2} }},")
        lines.append(f"  .data_size = sizeof({name}_map), .data = {name}_map }};")
        lines.append("")
    open(out, "w").write("\n".join(lines))
    print("wrote", out, len(circles), "images")


main()
