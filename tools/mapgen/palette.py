"""Distinct colours for labelled maps (provinces, regions, previews)."""
import colorsys

import numpy as np


def distinct_colors(count, seed=7):
    """`count` distinct colours spread over the hue circle by the golden ratio, never black or white."""
    rng = np.random.default_rng(seed)
    colors, used = [], {(0, 0, 0), (255, 255, 255)}
    golden = 0.618033988749895
    hue = rng.random()
    while len(colors) < count:
        hue = (hue + golden) % 1.0
        sat = 0.45 + 0.4 * rng.random()
        val = 0.55 + 0.4 * rng.random()
        color = tuple(int(c * 255) for c in colorsys.hsv_to_rgb(hue, sat, val))
        if color not in used:
            used.add(color)
            colors.append(color)
    return colors
