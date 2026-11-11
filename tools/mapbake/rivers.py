"""Rivers from rivers.png (drawn by hand in EU4's rivers.bmp convention, docs/MAPFORMAT.md): traced into smooth
polylines from source to mouth (bake.py: RVTX/RSEG), and the river corridor positions.py keeps clear of (`corridor`).

rivers.png, the size of land.png, transparent except the river lines, 1 px wide:
    (0, 255, 0)    source, the first pixel of a river
    (255, 0, 0)    join: the last pixel of a tributary, next to the river it flows into
    (255, 252, 0)  split (not used yet)
    blues          the river, by width class 3 (0, 225, 255, narrowest) .. 11 (0, 0, 100, widest)
A river ends where it reaches the sea or a lake (a few pixels short are bridged, pixels drawn on into the water
dropped) or at its join. A source drawn inside a lake, or at most SOURCE_BRIDGE px from one, is that lake's
outflow (drawn on from the lake). Lines step 4-connected as in EU4; a
diagonal step is taken too.

    python tools/mapbake/rivers.py      # check rivers.png: prints every river, its mouth and any problem
"""
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from generate_provinces import read_classes  # noqa: E402
from validate_land import SEA, LAKE  # noqa: E402

SOURCE, JOIN, SPLIT = (0, 255, 0), (255, 0, 0), (255, 252, 0)
WIDTH_CLASSES = {(0, 225, 255): 3, (0, 200, 255): 4, (0, 150, 255): 5, (0, 100, 255): 6, (0, 0, 255): 7,
                 (0, 0, 225): 8, (0, 0, 200): 9, (0, 0, 150): 10, (0, 0, 100): 11}
WIDTH = (2.6, 4.1)          # map px of the water at width class 3 and 11
BRIDGE = 4.0                # px: a river ending this close to the sea or a lake is drawn on to it
SOURCE_BRIDGE = 8.0         # px: a source this close to a lake flows out of it
SMOOTH = 2.0                # px: gaussian smoothing of the traced pixel path
WIDTH_SMOOTH = 10.0         # px: how gradually a river widens where its class changes
TAPER = 10.0                # px: a river narrows toward its source over this length
STEP = 1.0                  # px between the points of a traced river


def width_px(width_class):
    return WIDTH[0] + (WIDTH[1] - WIDTH[0]) * (width_class - 3) / 8.0


def load(path=ROOT / 'rivers.png'):
    rgba = np.asarray(Image.open(path).convert('RGBA'))
    return rgba[..., 3] > 128, rgba[..., :3].astype(int)


def trace(classes, path=ROOT / 'rivers.png'):
    """-> (rivers, problems). A river: dict(points (n, 2) map px, widths (n,) px, source 'land'|'lake', mouth
    'sea'|'lake'|'river'|'land', into: (river index, point index) for a tributary, lake: the lake pixel it ends in)."""
    mask, rgb = load(path)
    problems = []
    h, w = mask.shape
    labels, count = ndimage.label(mask, np.ones((3, 3), bool))
    sizes = np.bincount(labels.ravel(), minlength=count + 1)
    for k in np.flatnonzero(sizes[1:] < 3) + 1:
        y, x = np.argwhere(labels == k)[0]
        problems.append(f'{x},{y}: stray pixel(s), ignored')
        mask[labels == k] = False
    pixels = set(zip(*np.nonzero(mask)))
    adjacent = defaultdict(list)
    for y, x in pixels:
        for dy, dx in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            if (y + dy, x + dx) in pixels:
                adjacent[(y, x)].append((y + dy, x + dx))
        for dy, dx in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            if (y + dy, x + dx) in pixels and (y + dy, x) not in pixels and (y, x + dx) not in pixels:
                adjacent[(y, x)].append((y + dy, x + dx))
    colour = {p: tuple(rgb[p]) for p in pixels}
    water = (classes == SEA) | (classes == LAKE)
    to_water, nearest = ndimage.distance_transform_edt(~water, return_indices=True)

    def width_class(p):
        c = colour.get(p)
        if c is None:
            return None
        if c in WIDTH_CLASSES:
            return WIDTH_CLASSES[c]
        best = min(WIDTH_CLASSES, key=lambda k: sum((a - b) ** 2 for a, b in zip(k, c)))
        return WIDTH_CLASSES[best] if c not in (SOURCE, JOIN, SPLIT) else None

    walks = []
    for source in sorted((p for p in pixels if colour[p] == SOURCE), key=lambda p: (p[1], p[0])):
        path, joined = [source], None
        while True:
            cur = path[-1]
            if colour[cur] == JOIN and len(path) > 1:
                ahead = [q for q in adjacent[cur] if q not in path]
                joined = ahead[0] if ahead else None
                break
            ahead = [q for q in adjacent[cur] if q not in path[-3:]]
            if len(ahead) > 1:
                ahead = [q for q in ahead if colour[q] != JOIN] or ahead
            if len(ahead) > 1:
                problems.append(f'{cur[1]},{cur[0]}: the river forks (split), followed one branch')
            if not ahead:
                break
            path.append(ahead[0])
        walks.append((path, joined))

    rivers, where = [], {}
    for path, joined in walks:
        start = 0
        source = 'land'
        while start < len(path) - 1 and water[path[start]]:
            start += 1
            source = 'lake'
        path = path[max(start - 1, 0):]
        y, x = path[0]
        if source == 'land' and to_water[y, x] <= SOURCE_BRIDGE and classes[nearest[0][y, x], nearest[1][y, x]] == LAKE:
            ly, lx = nearest[0][y, x], nearest[1][y, x]
            steps = int(np.ceil(to_water[y, x]))
            path = [(int(round(ly + (y - ly) * s / steps)), int(round(lx + (x - lx) * s / steps)))
                    for s in range(steps)] + path
            source = 'lake'
        mouth, lake = 'land', None
        if joined is not None:
            mouth = 'river'
        else:
            wet = next((i for i in range(1, len(path)) if water[path[i]]), None)
            if wet is not None:
                path = path[:wet + 1]
            else:
                y, x = path[-1]
                if to_water[y, x] <= BRIDGE:
                    ny, nx = nearest[0][y, x], nearest[1][y, x]
                    steps = int(np.ceil(to_water[y, x]))
                    path = path + [(int(round(y + (ny - y) * s / steps)), int(round(x + (nx - x) * s / steps)))
                                   for s in range(1, steps + 1)]
                else:
                    problems.append(f'{x},{y}: river from {path[0][1]},{path[0][0]} ends on land, '
                                    f'{to_water[y, x]:.0f} px from water')
            if water[path[-1]]:
                mouth = 'sea' if classes[path[-1]] == SEA else 'lake'
                lake = path[-1]
        classes_along = [width_class(p) for p in path]
        known = [c for c in classes_along if c is not None] or [3]
        filled, last = [], known[0]
        for c in classes_along:
            last = c if c is not None else last
            filled.append(last)
        rivers.append(dict(pixels=path, classes=filled, source=source, mouth=mouth, lake=lake, joined=joined))
        for i, p in enumerate(path):
            where.setdefault(p, (len(rivers) - 1, i))

    for river in rivers:
        pts = np.array([(x + 0.5, y + 0.5) for y, x in river['pixels']], np.float64)
        widths = np.array([width_px(c) for c in river['classes']], np.float64)
        river['into'] = None
        if river['joined'] is not None:
            target = where.get(river['joined'])
            if target is None:
                y, x = river['joined']
                problems.append(f'{x},{y}: a join next to no river')
                river['mouth'] = 'land'
            else:
                river['into'] = target
                y, x = river['joined']
                pts = np.vstack([pts, [(x + 0.5, y + 0.5)]])
                widths = np.append(widths, widths[-1])
        if river['mouth'] in ('sea', 'lake'):
            # The last point on the waterline: halfway between the last dry pixel and the first wet one.
            pts[-1] = (pts[-1] + pts[-2]) / 2
        if river['source'] == 'lake':
            pts[0] = (pts[0] + pts[1]) / 2          # the outflow starts on the lake's shoreline
        river['points'], river['widths'] = resample(pts, widths)
    # A tributary's point index refers to its target's pixels: map it onto the resampled points.
    for river in rivers:
        if river['into'] is not None:
            k, i = river['into']
            target = rivers[k]
            px, py = target['pixels'][i][1] + 0.5, target['pixels'][i][0] + 0.5
            j = int(np.argmin(np.hypot(target['points'][:, 0] - px, target['points'][:, 1] - py)))
            river['into'] = (k, j)
            river['points'][-1] = target['points'][j]
    for river in rivers:
        del river['pixels'], river['classes'], river['joined']
    return rivers, problems


def resample(points, widths):
    """Smooth a pixel path (its ends stay put) and resample it every STEP px; widths along it, eased."""
    if len(points) > 3:
        smooth = ndimage.gaussian_filter1d(points, SMOOTH, axis=0, mode='nearest')
        pin = np.minimum(np.arange(len(points)), np.arange(len(points))[::-1]) / (2 * SMOOTH)
        weight = np.clip(pin, 0, 1)[:, None]
        points = points * (1 - weight) + smooth * weight
    s = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(points, axis=0).T))])
    n = max(int(np.ceil(s[-1] / STEP)), 1)
    t = np.linspace(0.0, s[-1], n + 1)
    out = np.stack([np.interp(t, s, points[:, 0]), np.interp(t, s, points[:, 1])], 1)
    width = ndimage.gaussian_filter1d(np.interp(t, s, widths), WIDTH_SMOOTH / STEP, mode='nearest')
    return out, width


def dense(rivers, spacing=0.5):
    """(points (n, 2), half width (n,)) sampled every `spacing` px along all rivers."""
    pts, hw = [], []
    for river in rivers:
        p, w = river['points'], river['widths']
        s = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(p, axis=0).T))])
        t = np.linspace(0.0, s[-1], max(int(s[-1] / spacing), 1) + 1)
        pts.append(np.stack([np.interp(t, s, p[:, 0]), np.interp(t, s, p[:, 1])], 1))
        hw.append(np.interp(t, s, w) / 2)
    return np.vstack(pts), np.concatenate(hw)


def corridor(shape, rivers, pad):
    """Pixels within `pad` px of any river's water."""
    pts, half = dense(rivers)
    near = np.zeros(shape, bool)
    xs = np.clip(pts[:, 0].astype(int), 0, shape[1] - 1)
    ys = np.clip(pts[:, 1].astype(int), 0, shape[0] - 1)
    near[ys, xs] = True
    reach = float(half.max()) + pad + 1
    candidates = ndimage.distance_transform_edt(~near) <= reach
    yy, xx = np.nonzero(candidates)
    d, k = cKDTree(pts).query(np.stack([xx + 0.5, yy + 0.5], 1))
    out = np.zeros(shape, bool)
    out[yy, xx] = d <= half[k] + pad
    return out


def traced():
    """The rivers of rivers.png (none if it is missing), traced; problems are printed, not fatal."""
    if not (ROOT / 'rivers.png').exists():
        return []
    rivers, problems = trace(read_classes(ROOT / 'land.png'))
    for problem in problems:
        print('rivers.png:', problem)
    return rivers


def main():
    classes = read_classes(ROOT / 'land.png')
    rivers, problems = trace(classes)
    for k, r in enumerate(rivers):
        p = r['points']
        into = f' -> river {r["into"][0]}' if r['into'] else ''
        print(f'river {k}: {p[0][0]:.0f},{p[0][1]:.0f} ({r["source"]}) -> {p[-1][0]:.0f},{p[-1][1]:.0f} '
              f'({r["mouth"]}{into}), {len(p)} points, width {r["widths"].min():.1f}..{r["widths"].max():.1f} px')
    for problem in problems:
        print('problem:', problem)


if __name__ == '__main__':
    main()
