"""Province names, baked: EU4's map-name placement (archeologyEU4/Labels/LabelPython, the verified reference of
code/src/core/labels.cpp) with every province as its own group, the way SetMapNameMode's keyed modes build groups:
one pass over the provinces with pixels in ascending id, the identifier being the province itself, named after it.
The engine shows these names instead of the map mode's names from the 10th zoom level on.

Land names first, exactly as EU4 places them. Sea and lake names rank below them: a water name may cover neither land
nor a land name (nor a water name placed before it). It is placed by the same algorithm on its province with the
pixels near land and near the names already placed taken away (records with EU4's water flags: no water between,
water ends allowed); while its glyphs still touch land or a name, the margin grows (so the name gets smaller and moves
into open water). A water name that cannot fit, or would shrink below WATER_MIN_SCALE, is left out.

    python tools/labels/provinces.py     # -> build/labels/province_names.npz (glyphs, their province), after
                                         #    tools/fonts/mapfont.py; tools/mapbake/positions.py keeps cities off them
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reference import ROOT, map_data, map_font, to_map  # noqa: E402
from placement.groups import Group, Groups  # noqa: E402
from placement.pipeline import place_labels  # noqa: E402

WATER_PAD = 2.0          # map px between a water name's glyph quads and land or another name
WATER_MARGINS = [0, 2, 4, 6, 9, 12, 16, 20, 25, 30, 36, 44, 52, 62, 74]   # px taken off the province, tried in turn
WATER_MIN_SCALE = 0.04   # map px per font px: a water name squeezed smaller than this is left out
WATER_SHRINK = [1.0, 0.85, 0.7, 0.55]   # factors on EU4's letter size tried for each margin


def record(data, provinces, pid, gid):
    # The 45-byte strncpy of SProvinceData, terminated safely (as LabelPython does).
    name = provinces[pid]['name'].encode('cp1252', errors='replace')[:45].decode('cp1252')
    group = Group(gid, str(pid), pid, 'province', [pid], name, [])
    return group, (pid, gid, name, bool(data.land[pid]), not bool(data.land[pid]))


def corners(glyph):
    x, y, dx, dy, w, h, _ = glyph
    ux, uy = dy, -dx
    return [(x + dx * a * w / 2 + ux * b * h / 2, y + dy * a * w / 2 + uy * b * h / 2)
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]


def quads(glyphs, shape, origin=(0, 0)):
    """Pixels covered by glyph quads (map px rows: x, y, reading direction x, y, width, height, code) in a window of
    `shape` whose top left is map px `origin` (x, y)."""
    image = Image.new('L', (shape[1], shape[0]))
    draw = ImageDraw.Draw(image)
    for glyph in glyphs:
        draw.polygon([(x - origin[0], y - origin[1]) for x, y in corners(glyph)], fill=255)
    return np.asarray(image) > 0


def touches(glyphs, forbidden):
    """Whether any glyph quad covers a pixel of the map px mask `forbidden` (or leaves the map)."""
    points = np.array([c for glyph in glyphs for c in corners(glyph)])
    x0, y0 = np.floor(points.min(0)).astype(int) - 1
    x1, y1 = np.ceil(points.max(0)).astype(int) + 2
    h, w = forbidden.shape
    if x0 < 0 or y0 < 0 or x1 > w or y1 > h:
        return True
    return bool((quads(glyphs, (y1 - y0, x1 - x0), (x0, y0)) & forbidden[y0:y1, x0:x1]).any())


def grown(mask, pad):
    return ndimage.distance_transform_edt(~mask) <= pad if mask.any() else mask


def largest(mask):
    labels, count = ndimage.label(mask)
    return mask if count <= 1 else labels == np.argmax(np.bincount(labels.ravel())[1:]) + 1


class Shrunk:
    """The map font with the letters scaled down by `factor` (EU4's area and length terms both), centred on the same
    baseline."""

    def __init__(self, font, factor):
        self.font, self.factor = font, factor

    def layout(self, name, baseline, area, length):
        return self.font.layout(name, baseline, area * self.factor ** 2, length * self.factor)


def place_one(data, font, group, rec):
    table = np.full(data.count, -1, np.int32)
    table[rec[0]] = group.id
    labels, _ = place_labels(data, Groups([group], [rec], table, [], [], []), font)
    return labels[0] if labels else None


def place_water(data, provinces, font, pid, named, forbidden, shore):
    """The water province's name clear of `forbidden` (map px: land and the names placed, `named`, both grown by
    WATER_PAD), or None. The province
    loses its pixels within a margin of land and names (or keeps only its largest piece then) and the letters shrink;
    of the names that fit, the one with the largest letters."""
    height = data.pixels.shape[0]
    raster = data.pixels                                   # bottom-up; this province's pixels are edited and restored
    own = raster == pid
    ys, xs = np.nonzero(own)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    window = (slice(y0, y1), slice(x0, x1))
    own_w = own[window]
    shore_w = shore[::-1][window]                          # distance to land, bottom-up like the raster
    names_w = named[::-1][window]
    saved = (data.moments[pid].copy(), data.bbox[pid].copy())
    group, rec = record(data, provinces, pid, 0)           # a group of its own
    best = None
    try:
        for margin in WATER_MARGINS:
            free = own_w & (shore_w > margin + WATER_PAD) & ~grown(names_w, margin + WATER_PAD)
            if free.sum() < 16:
                break
            piece = largest(free)
            for shape in ([free, piece] if piece.sum() < free.sum() else [free]):
                raster[window][own_w & ~shape] = 0
                fy, fx = np.nonzero(shape)
                fx, fy = (fx + x0).astype(np.int64), (fy + y0).astype(np.int64)
                data.moments[pid] = np.array([len(fx), fx.sum(), fy.sum(), (fx * fy).sum(), (fx * fx).sum(),
                                              (fy * fy).sum()], np.uint64)
                data.bbox[pid] = [fx.min(), fy.min(), fx.max() + 1, fy.max() + 1]
                for factor in WATER_SHRINK:
                    label = place_one(data, Shrunk(font, factor), group, rec)
                    if label is None or label['scale'] < WATER_MIN_SCALE or                             (best is not None and label['scale'] <= best['scale']):
                        break
                    if not touches(to_map(label, height), forbidden):
                        best = label
                        break
                raster[window][own_w] = pid
    finally:
        raster[window][own_w] = pid
        data.moments[pid], data.bbox[pid] = saved
    return best


def main():
    data, provinces = map_data()
    font = map_font()
    height = data.pixels.shape[0]
    used = [pid for pid in np.flatnonzero(data.moments[:, 0]).tolist() if pid != 0]

    groups, records = [], []
    for pid in used:
        if data.land[pid]:
            group, rec = record(data, provinces, pid, len(groups))
            groups.append(group)
            records.append(rec)
    table = np.full(data.count, -1, np.int32)
    for pid, gid, *_ in records:
        table[pid] = gid
    labels, attempts = place_labels(data, Groups(groups, records, table, [], [], []), font)
    failed = [provinces[a['group']['seed']]['name'] for a in attempts if not a['success']]

    ids = data.pixels[::-1]
    land = data.land[ids]
    shore = ndimage.distance_transform_edt(~land)          # map px to the nearest land pixel
    named = quads([g for label in labels for g in to_map(label, height)], ids.shape)
    forbidden = (shore <= WATER_PAD) | grown(named, WATER_PAD)
    water_named, water_failed = 0, []
    for pid in used:
        if data.land[pid]:
            continue
        label = place_water(data, provinces, font, pid, named, forbidden, shore)
        if label is None:
            water_failed.append(provinces[pid]['name'])
            continue
        labels.append(label)
        water_named += 1
        glyphs = to_map(label, height)
        points = np.array([c for glyph in glyphs for c in corners(glyph)])
        x0, y0 = np.maximum(np.floor(points.min(0)).astype(int) - 4, 0)
        x1, y1 = np.ceil(points.max(0)).astype(int) + 5
        window = (slice(y0, y1), slice(x0, x1))
        quad = quads(glyphs, named[window].shape, (x0, y0))
        named[window] |= quad
        forbidden[window] |= grown(quad, WATER_PAD)

    glyphs = np.array([g for label in labels for g in to_map(label, height)], np.float32).reshape(-1, 7)
    province = np.array([label['group'].seed
                         for label in labels for _ in label['glyphs']], np.int32)
    out = ROOT / 'build' / 'labels'
    out.mkdir(parents=True, exist_ok=True)
    np.savez(out / 'province_names.npz', glyphs=glyphs, province=province)
    print(f'{out / "province_names.npz"}: {len(labels) - water_named} of {len(records)} land and {water_named} of '
          f'{water_named + len(water_failed)} water provinces named, {len(glyphs)} glyphs'
          + (f'; no name for land {failed}' if failed else '') + (f'; water {water_failed}' if water_failed else ''))


if __name__ == '__main__':
    sys.exit(main())
