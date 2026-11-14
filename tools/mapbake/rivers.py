"""Rivers for the map file: the block paths of rivers.png (tools/mapgen/river_paths.py) as smooth polylines in map
pixels from source to mouth (bake.py: RVTX/RSEG), and the river corridor positions.py keeps clear of (`corridor`).

A path runs through the centres of its blocks; a mouth ends on the waterline (halfway into the water block it
touches), a lake's outflow starts on the lake's shoreline, a tributary ends on the river it joins. The water is as wide
as the river's blocks: a brook one block, a river two.

    python tools/mapbake/rivers.py      # prints every river, its mouth and any problem
"""
import sys
from pathlib import Path

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from heightmap import BLOCK, read_blocks  # noqa: E402
from river_paths import EIGHT_STEPS, inside, read_lines, trace as trace_blocks  # noqa: E402
from validate_land import LAKE, SEA  # noqa: E402

SMOOTH = 2.0                # px: gaussian smoothing of the path through block centres
WIDTH_SMOOTH = 10.0         # px: how gradually a river widens where it changes from brook to river
STEP = 1.0                  # px between the points of a traced river


def centre(block):
    y, x = block
    return (x + 0.5) * BLOCK, (y + 0.5) * BLOCK


def water_beside(classes, block):
    """The first sea or lake block next to `block` (sides before corners), or None."""
    y, x = block
    for dy, dx in EIGHT_STEPS:
        if inside(y + dy, x + dx) and classes[y + dy, x + dx] in (SEA, LAKE):
            return y + dy, x + dx
    return None


def trace(classes):
    """-> (rivers, problems). A river: dict(points (n, 2) map px, widths (n,) px, source 'land' | 'lake', mouth
    'sea' | 'lake' | 'river' | 'land', into: (river index, point index) for a tributary)."""
    kind, problems = read_lines()
    if problems:
        return [], problems
    paths, found = trace_blocks(kind, classes)
    problems = [f'{x},{y}: {text}' for x, y, text in found]
    rivers = []
    for path in paths:
        pts = np.array([centre(p) for p in path['path']], np.float64)
        widths = np.array([w * BLOCK for w in path['width']], np.float64)
        if path['source'] == 'lake':
            lake = water_beside(classes, path['path'][0])
            if lake is not None:
                pts = np.vstack([(np.array(centre(lake)) + pts[0]) / 2, pts])
                widths = np.concatenate([widths[:1], widths])
        if path['mouth'] in ('sea', 'lake'):
            water = water_beside(classes, path['path'][-1])
            if water is not None:
                pts = np.vstack([pts, (np.array(centre(water)) + pts[-1]) / 2])
                widths = np.append(widths, widths[-1])
        rivers.append(dict(points=pts, widths=widths, source=path['source'], mouth=path['mouth'] or 'land',
                           into=path['into']))
    for river in rivers:
        if river['into'] is not None:
            target = rivers[river['into']]
            end = river['points'][-1]
            j = int(np.argmin(np.hypot(*(target['points'] - end).T)))
            river['points'] = np.vstack([river['points'], target['points'][j]])
            river['widths'] = np.append(river['widths'], river['widths'][-1])
    for river in rivers:
        river['points'], river['widths'] = resample(river['points'], river['widths'])
    for river in rivers:
        if river['into'] is not None:
            target = rivers[river['into']]
            end = river['points'][-1]
            j = int(np.argmin(np.hypot(*(target['points'] - end).T)))
            river['into'] = (river['into'], j)
            river['points'][-1] = target['points'][j]
    return rivers, problems


def resample(points, widths):
    """Smooth a path (its ends stay put) and resample it every STEP px; widths along it, eased."""
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
    if not rivers:
        return np.zeros(shape, bool)
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
    classes, _, _ = read_blocks()
    rivers, problems = trace(classes)
    for problem in problems:
        print('rivers.png:', problem)
    return rivers


def main():
    classes, _, _ = read_blocks()
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
