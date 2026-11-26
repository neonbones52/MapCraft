"""The armies' march paths between neighbouring land provinces: a curve per step that keeps to the land of the two
provinces (docs/GAMEDATA.md, "Движение армий"); bake.py puts them into the map (MPTO, MPTV, MPTL), and the step's cost
is the curve's length times the movement_cost of the province it goes into.

For each pair of land neighbours, from the unit point of one to the unit point of the other:
  1. the cheapest 8-connected way over the pixels of the two provinces (sea and other provinces are not crossed): a
     pixel costs 1, more within EDGE_MARGIN px of what may not be crossed (up to 1 + EDGE_PENALTY at its edge), so
     the way keeps to the middle of the land where there is room and squeezes through a narrow border only where it
     must; river pixels cost RIVER_PENALTY more, so a way crosses a river rather than following it; decorative lakes
     (land in the game, docs/MAPFORMAT.md) LAKE_PENALTY more, so a way wades through one only where the pair has no
     dry border (Honeyford's with Meadowbay and Rabbit Haven);
  2. simplified (Douglas-Peucker, SIMPLIFY px), each piece kept only if it stays on the pair's land;
  3. smoothed by corner cutting (Chaikin, SMOOTHING rounds), a round kept only if every piece stays on the land.
The curve of b -> a is a -> b's reversed (the same length). A pair whose unit points the pair's land does not join (a
province touching the other only with a piece cut off from its unit point) has no step: it is printed.

    python tools/mapbake/marches.py     # a summary only; bake.py builds the paths itself
"""
import math
import sys
from pathlib import Path

import numpy as np
from scipy import ndimage
from skimage.graph import route_through_array

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from heightmap import BLOCK  # noqa: E402
from sources import province_map, read_csv  # noqa: E402
from surface import RIVER, surface  # noqa: E402
from validate_land import LAKE, LAND  # noqa: E402

EDGE_MARGIN = 10.0    # px from what may not be crossed within which a pixel costs more
EDGE_PENALTY = 3.0    # the extra cost of a pixel at that edge
RIVER_PENALTY = 2.0   # the extra cost of a river pixel
LAKE_PENALTY = 30.0   # the extra cost of a decorative lake's pixel
SIMPLIFY = 2.0        # px the simplified curve may leave the way
SMOOTHING = 3         # corner cutting rounds


def inside(walk, a, b):
    """Whether the segment a -> b (x, y in map px) stays on the walkable pixels (sampled every half pixel)."""
    steps = max(2, int(math.ceil(math.hypot(b[0] - a[0], b[1] - a[1]) * 2)) + 1)
    t = np.linspace(0.0, 1.0, steps)
    xs = np.floor(a[0] + (b[0] - a[0]) * t).astype(int)
    ys = np.floor(a[1] + (b[1] - a[1]) * t).astype(int)
    ok = (xs >= 0) & (ys >= 0) & (xs < walk.shape[1]) & (ys < walk.shape[0])
    return bool(ok.all() and walk[ys, xs].all())


def simplify(points, walk):
    """Douglas-Peucker within SIMPLIFY px; a piece that would leave the land is split further."""
    keep = np.zeros(len(points), bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        a, b = points[i], points[j]
        d = b - a
        norm = math.hypot(d[0], d[1])
        rel = points[i + 1:j] - a
        dist = np.abs(rel[:, 0] * d[1] - rel[:, 1] * d[0]) / norm if norm > 0 else np.hypot(rel[:, 0], rel[:, 1])
        k = int(np.argmax(dist))
        if dist[k] > SIMPLIFY or not inside(walk, a, b):
            keep[i + 1 + k] = True
            stack += [(i, i + 1 + k), (i + 1 + k, j)]
    return points[keep]


def smooth(points, walk):
    """Chaikin's corner cutting (each piece's quarter points; the ends stay); a round that would leave the land is
    not taken."""
    for _ in range(SMOOTHING):
        if len(points) < 3:
            break
        cut = [points[0]]
        for a, b in zip(points[:-1], points[1:]):
            cut += [0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b]
        cut.append(points[-1])
        cut = np.array(cut)
        if not all(inside(walk, cut[i], cut[i + 1]) for i in range(len(cut) - 1)):
            break
        points = cut
    return points


def length(points):
    return float(np.hypot(*np.diff(points, axis=0).T).sum())


def land_pairs(provinces):
    """The neighbours (adjacency.csv, a < b) that are both land."""
    land = {pid for pid, row in provinces.items() if row['type'] == 'land'}
    return sorted({(min(a, b), max(a, b)) for a, b in ((int(r['a']), int(r['b']))
                                                      for r in read_csv(ROOT / 'adjacency.csv'))
                   if a in land and b in land})


def build(ids, pairs, unit, blocks):
    """-> {(a, b): float32 array (k, 2) of map px points from a's unit point to b's}, the pairs without a way.

    ids: province per map pixel; pairs: land_pairs(); unit: per province, its unit point (x, y); blocks: surface()
    (its cls marks rivers)."""
    cls = blocks['cls'].repeat(BLOCK, 0).repeat(BLOCK, 1)
    ground = (cls == LAND) | (cls == RIVER) | (cls == LAKE)
    river = cls == RIVER
    lake = cls == LAKE
    boxes = ndimage.find_objects(ids)
    paths, failed = {}, []
    for p, q in pairs:
        bp, bq = boxes[p - 1], boxes[q - 1]
        y0 = max(min(bp[0].start, bq[0].start) - 1, 0)
        x0 = max(min(bp[1].start, bq[1].start) - 1, 0)
        y1 = min(max(bp[0].stop, bq[0].stop) + 1, ids.shape[0])
        x1 = min(max(bp[1].stop, bq[1].stop) + 1, ids.shape[1])
        local = ids[y0:y1, x0:x1]
        walk = ((local == p) | (local == q)) & ground[y0:y1, x0:x1]
        # Room: the distance to what may not be crossed, or to a lake's water.
        room = ndimage.distance_transform_edt(np.pad(walk & ~lake[y0:y1, x0:x1], 1))[1:-1, 1:-1]
        cost = 1.0 + EDGE_PENALTY * np.clip((EDGE_MARGIN - room) / EDGE_MARGIN, 0.0, 1.0) + \
            RIVER_PENALTY * river[y0:y1, x0:x1] + LAKE_PENALTY * lake[y0:y1, x0:x1]
        cost[~walk] = np.inf
        start = (int(unit[p][1]) - y0, int(unit[p][0]) - x0)
        end = (int(unit[q][1]) - y0, int(unit[q][0]) - x0)
        if not (walk[start] and walk[end]):
            failed.append((p, q))
            continue
        try:
            cells, total = route_through_array(cost, start, end, fully_connected=True, geometric=True)
        except ValueError:
            failed.append((p, q))
            continue
        if not np.isfinite(total):
            failed.append((p, q))
            continue
        points = np.array([(c + 0.5, r + 0.5) for r, c in cells], np.float64)
        points[0] = (unit[p][0] - x0, unit[p][1] - y0)
        points[-1] = (unit[q][0] - x0, unit[q][1] - y0)
        points = smooth(simplify(points, walk), walk)
        paths[(p, q)] = (points + (x0, y0)).astype(np.float32)
    return paths, failed


def main():
    ids, provinces = province_map()
    unit = np.full((max(provinces) + 1, 2), np.nan)
    for r in read_csv(ROOT / 'positions.csv'):
        unit[int(r['id'])] = (float(r['unit_x']), float(r['unit_y']))
    paths, failed = build(ids, land_pairs(provinces), unit, surface())
    lengths = sorted(length(v) for v in paths.values())
    straight = sorted(math.dist(unit[p], unit[q]) for p, q in paths)
    print(f'{len(paths)} march paths, {sum(len(v) for v in paths.values())} points; length median '
          f'{lengths[len(lengths) // 2]:.1f} px (straight {straight[len(straight) // 2]:.1f}), max {lengths[-1]:.1f}')
    if failed:
        print(f'no way within the pair: {failed}')


if __name__ == '__main__':
    main()
