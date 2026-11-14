"""Our map in the form of the verified Python port of EU4's map-name algorithm (archeologyEU4/Labels/LabelPython):
the province raster bottom-up, the provinces' data, the label font's Mapfont-style metrics (build/fonts/mapfont.fnt,
tools/fonts/mapfont.py) and the glyphs of a placed label in map px. LabelPython's modules run unchanged; the only
adaptation is the neighbour scan without EU4's x = W-1 -> 0 seam, as our map is flat, not a globe. Used by
tools/labels/provinces.py (the baked province names).
"""
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from numba import njit

ROOT = Path(__file__).resolve().parents[2]
REFERENCE = ROOT.parent / 'eu4' / 'archeologyEU4' / 'Labels' / 'LabelPython'
sys.path.insert(0, str(REFERENCE))
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from rendering.font import MapFont  # noqa: E402
from sources import province_map  # noqa: E402



@njit(cache=True)
def precompute(pixels, count):
    """LabelPython eu4data.map.precompute, without the horizontal seam."""
    h, w = pixels.shape
    moments = np.zeros((count, 6), np.uint64)
    bbox = np.zeros((count, 4), np.int32)
    bbox[:, 0] = w
    bbox[:, 1] = h
    seen = np.zeros((count, count), np.uint8)
    neighbours = [list(np.empty(0, np.int32)) for _ in range(count)]
    for y in range(h):
        for x in range(w):
            a = int(pixels[y, x])
            if a > 0:
                moments[a, 0] += np.uint64(1)
                moments[a, 1] += np.uint64(x)
                moments[a, 2] += np.uint64(y)
                moments[a, 3] += np.uint64(np.int64(np.int32(x*y)))
                moments[a, 4] += np.uint64(np.int64(np.int32(x*x)))
                moments[a, 5] += np.uint64(np.int64(np.int32(y*y)))
                bbox[a, 0] = min(bbox[a, 0], x)
                bbox[a, 1] = min(bbox[a, 1], y)
                bbox[a, 2] = max(bbox[a, 2], x+1)
                bbox[a, 3] = max(bbox[a, 3], y+1)
            for axis in range(2):
                if axis == 1 and x + 1 >= w:
                    continue
                b = int(pixels[min(y+1, h-1), x]) if axis == 0 else int(pixels[y, x+1])
                if a != b:
                    if a > 0 and seen[b, a] == 0:
                        neighbours[b].append(np.int32(a))
                        seen[b, a] = 1
                    if b > 0 and seen[a, b] == 0:
                        neighbours[a].append(np.int32(b))
                        seen[a, b] = 1
    for pid in range(count):
        if moments[pid, 0] == 0:
            bbox[pid] = 0
    return moments, bbox, neighbours


def map_data():
    """-> (LabelPython-style map data of our map, provinces.csv rows by id)."""
    ids, provinces = province_map()
    count = max(provinces) + 1
    pixels = np.ascontiguousarray(ids[::-1].astype(np.uint16))          # EU4: the bottom row first
    moments, bbox, neighbours = precompute(pixels, count)
    land = np.zeros(count, np.bool_)
    lake = np.zeros(count, np.bool_)
    for pid, row in provinces.items():
        land[pid] = row['type'] in ('land', 'wasteland')
        lake[pid] = row['type'] == 'lake'
    data = SimpleNamespace(pixels=pixels, moments=moments, bbox=bbox, neighbours=[list(n) for n in neighbours],
                           land=land, lake=lake, impassable=np.zeros(count, np.bool_), regions=[None] * count,
                           areas=[None] * count, superregions={}, localization={}, min_name=2, min_region=4,
                           count=count)
    return data, provinces


def map_font():
    return MapFont(ROOT / 'build' / 'fonts' / 'mapfont.fnt', ROOT / 'blockyfont.otf')   # the ttf only draws previews


def to_map(label, height):
    """A LabelPython label's glyphs in our map px: x, y, reading direction, quad size, code."""
    return [(float(g['position'][0]) + 0.5, height - 0.5 - float(g['position'][1]), float(g['tangent'][0]),
             -float(g['tangent'][1]), float(g['width']), float(g['height']), g['char'].encode('cp1252')[0])
            for g in label['glyphs']]
