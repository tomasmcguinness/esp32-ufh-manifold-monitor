#!/usr/bin/env python3
"""Prototype of the float detection, run on frames saved from the board.

For each tube: take a vertical strip around the rod, above the red cap, and score
every row by its mean "redness" (R - max(G, B)). The float ring is the strongest
red band in the strip; its row, refined to sub-pixel with a centroid, is the reading.
Flow is linear in that row: flow = (row - zero_row) / px_per_lpm.

This is what main/flow_reader.c does on the board; keep the two in step.

    python3 tools/flow_prototype.py tools/fixtures/*.jpg

Fixtures (2560x1920 frames from the board, 2026-11-06):
    zero-flow.jpg    valve shut, all loops at 0 L/min
    all-1.1lpm.jpg   all three loops read ~1.1 L/min by eye
The firmware's default calibration in main/flow_reader.c comes from these two frames.
"""
import json
import sys

import numpy as np
from PIL import Image

# Per-tube strips in full-resolution (2560x1920) pixels, for the camera position of
# 2026-11-06. x0..x1 spans the rod; y0..y1 runs from above the 0 mark to just above the cap.
TUBES = [
    {"name": "Loop 1", "x0": 1040, "x1": 1090, "y0": 150, "y1": 300},
    {"name": "Loop 2", "x0": 1307, "x1": 1357, "y0": 150, "y1": 300},
    {"name": "Loop 3", "x0": 1571, "x1": 1621, "y0": 150, "y1": 300},
]


def ring_row(rgb, t):
    """Return (row, peak_strength) of the float ring inside tube t's strip."""
    strip = rgb[t["y0"]:t["y1"], t["x0"]:t["x1"]].astype(np.int32)
    redness = strip[..., 0] - np.maximum(strip[..., 1], strip[..., 2])
    profile = np.clip(redness, 0, None).mean(axis=1)
    # 5-row box smoothing so JPEG noise in single rows cannot win.
    smooth = np.convolve(profile, np.ones(5) / 5, mode="same")
    peak = int(np.argmax(smooth))
    lo, hi = max(peak - 4, 0), min(peak + 5, len(profile))
    w = profile[lo:hi]
    centroid = lo + float((w * np.arange(len(w))).sum() / max(w.sum(), 1e-6))
    return t["y0"] + centroid, float(smooth[peak])


def cap_top(rgb, t, search=(280, 380)):
    """Row where the solid red cap starts below tube t: the anchor that makes the
    reading immune to the camera shifting a few pixels."""
    strip = rgb[search[0]:search[1], t["x0"]:t["x1"]].astype(np.int32)
    redness = np.clip(strip[..., 0] - np.maximum(strip[..., 1], strip[..., 2]), 0, None).mean(axis=1)
    level = 0.5 * redness.max()          # half-way up the step from tube to cap
    return search[0] + int(np.argmax(redness > level))


def measure(path):
    """Per tube: (ring height above the cap in px, ring strength)."""
    rgb = np.asarray(Image.open(path).convert("RGB"))
    out = []
    for t in TUBES:
        row, strength = ring_row(rgb, t)
        out.append((cap_top(rgb, t) - row, strength))
    return out


if __name__ == "__main__":
    for path in sys.argv[1:]:
        rows = measure(path)
        print(f"{path.split('/')[-1]:55s}", "  ".join(f"{h:6.1f} (s={s:4.1f})" for h, s in rows))
