"""Province borders as smooth polylines, the way EU4 builds its border strips (archeologyEU4/Render/PROVINCES.md §7).

Boundaries run along pixel edges. Every run between two junctions (lattice corners where three or more provinces
meet) or around a closed loop becomes one chain for one pair of provinces. A chain's points are its edge midpoints
(a pixel staircase along a diagonal has collinear midpoints), junction corners stay fixed at the ends so chains meet
exactly. Then, like EU4's SmoothBorders, the points between two water provinces are box-averaged (EU4: two radius-6
passes; here too, the window shrinking toward the ends); a border touching land is not smoothed, it runs along the
block edges (provinces are cut on the block grid). Finally the chains are simplified (Douglas-Peucker).

    python tools/mapbake/borders.py      # preview: build/map/borders_preview.png
"""
import sys
from pathlib import Path

import numpy as np
from numba import njit

ROOT = Path(__file__).resolve().parents[2]
LAND_RADIUS = 0          # box-average radius (points) for borders touching land: none, they follow block edges
WATER_RADIUS = 6         # between two water provinces (EU4's value)
PASSES = 2
TOLERANCE = 0.08         # px: Douglas-Peucker tolerance
CLOSED = 1               # chain flag


@njit(cache=True)
def _has(hb, vb, cy, cx, d):
    if d == 0:
        return cx < hb.shape[1] and hb[cy, cx]
    if d == 2:
        return cx > 0 and hb[cy, cx - 1]
    if d == 1:
        return cy < vb.shape[0] and vb[cy, cx]
    return cy > 0 and vb[cy - 1, cx]


@njit(cache=True)
def _visit(hv, vv, cy, cx, d):
    """Mark the edge leaving corner (cy, cx) in direction d; return True if it was already marked."""
    if d == 0:
        seen = hv[cy, cx]
        hv[cy, cx] = True
    elif d == 2:
        seen = hv[cy, cx - 1]
        hv[cy, cx - 1] = True
    elif d == 1:
        seen = vv[cy, cx]
        vv[cy, cx] = True
    else:
        seen = vv[cy - 1, cx]
        vv[cy - 1, cx] = True
    return seen


@njit(cache=True)
def _pair(ids, cy, cx, d):
    if d == 0:
        a, b = ids[cy - 1, cx], ids[cy, cx]
    elif d == 2:
        a, b = ids[cy - 1, cx - 1], ids[cy, cx - 1]
    elif d == 1:
        a, b = ids[cy, cx - 1], ids[cy, cx]
    else:
        a, b = ids[cy - 1, cx - 1], ids[cy - 1, cx]
    return min(a, b), max(a, b)


@njit(cache=True)
def trace(ids):
    """Chains of edge midpoints between junctions and closed loops. Returns flat xs, ys and per chain
    first, count, a, b, closed."""
    h, w = ids.shape
    hb = np.zeros((h + 1, w), np.bool_)      # edge from corner (cy, cx) to (cy, cx + 1)
    vb = np.zeros((h, w + 1), np.bool_)      # edge from corner (cy, cx) to (cy + 1, cx)
    for y in range(1, h):
        for x in range(w):
            hb[y, x] = ids[y - 1, x] != ids[y, x]
    for y in range(h):
        for x in range(1, w):
            vb[y, x] = ids[y, x - 1] != ids[y, x]
    degree = np.zeros((h + 1, w + 1), np.uint8)
    for cy in range(h + 1):
        for cx in range(w + 1):
            n = 0
            for d in range(4):
                if _has(hb, vb, cy, cx, d):
                    n += 1
            degree[cy, cx] = n
    hv = np.zeros_like(hb)
    vv = np.zeros_like(vb)
    edges = hb.sum() + vb.sum()
    xs = np.empty(edges + 2 * edges // 2 + 16, np.float32)
    ys = np.empty_like(xs)
    chains = np.empty((edges + 16, 5), np.int64)
    n_points = 0
    n_chains = 0
    dx = np.array([1, 0, -1, 0])
    dy = np.array([0, 1, 0, -1])
    for pass_ in range(2):          # 0: chains from junctions, 1: closed loops
        for cy in range(h + 1):
            for cx in range(w + 1):
                if pass_ == 0 and degree[cy, cx] < 3:
                    continue
                if pass_ == 1 and degree[cy, cx] != 2:
                    continue
                for d0 in range(4):
                    if not _has(hb, vb, cy, cx, d0):
                        continue
                    if _visit(hv, vv, cy, cx, d0):
                        continue
                    a, b = _pair(ids, cy, cx, d0)
                    first = n_points
                    if pass_ == 0:
                        xs[n_points] = cx
                        ys[n_points] = cy
                        n_points += 1
                    y, x, d = cy, cx, d0
                    while True:
                        xs[n_points] = x + 0.5 * dx[d]
                        ys[n_points] = y + 0.5 * dy[d]
                        n_points += 1
                        y += dy[d]
                        x += dx[d]
                        if degree[y, x] != 2:
                            xs[n_points] = x
                            ys[n_points] = y
                            n_points += 1
                            break
                        if pass_ == 1 and y == cy and x == cx:
                            break
                        back = (d + 2) % 4
                        nd = -1
                        for k in range(4):
                            if k != back and _has(hb, vb, y, x, k):
                                nd = k
                                break
                        d = nd
                        if _visit(hv, vv, y, x, d):
                            break
                    chains[n_chains, 0] = first
                    chains[n_chains, 1] = n_points - first
                    chains[n_chains, 2] = a
                    chains[n_chains, 3] = b
                    chains[n_chains, 4] = pass_
                    n_chains += 1
    return xs[:n_points], ys[:n_points], chains[:n_chains]


@njit(cache=True)
def _smooth(xs, ys, closed, radius, passes):
    n = xs.shape[0]
    for _ in range(passes):
        sx = xs.copy()
        sy = ys.copy()
        for i in range(n):
            r = radius if closed else min(radius, i, n - 1 - i)
            if r == 0:
                continue
            tx = 0.0
            ty = 0.0
            for k in range(-r, r + 1):
                j = (i + k) % n
                tx += xs[j]
                ty += ys[j]
            sx[i] = tx / (2 * r + 1)
            sy[i] = ty / (2 * r + 1)
        xs, ys = sx, sy
    return xs, ys


@njit(cache=True)
def _simplify(xs, ys, tolerance):
    n = xs.shape[0]
    keep = np.zeros(n, np.bool_)
    keep[0] = True
    keep[n - 1] = True
    stack = [(0, n - 1)]
    while len(stack) > 0:
        i0, i1 = stack.pop()
        if i1 - i0 < 2:
            continue
        ax, ay = xs[i0], ys[i0]
        bx, by = xs[i1], ys[i1]
        vx, vy = bx - ax, by - ay
        length = np.sqrt(vx * vx + vy * vy)
        worst = -1.0
        index = -1
        for i in range(i0 + 1, i1):
            if length > 1e-6:
                dist = abs((xs[i] - ax) * vy - (ys[i] - ay) * vx) / length
            else:
                dist = np.sqrt((xs[i] - ax) ** 2 + (ys[i] - ay) ** 2)
            if dist > worst:
                worst = dist
                index = i
        if worst > tolerance:
            keep[index] = True
            stack.append((i0, index))
            stack.append((index, i1))
    return keep


def build(ids, water):
    """ids: province id per pixel; water: bool per province id. Returns (points f32[m, 2], chains u32[k, 4] with
    first, count, a | b << 16, flags)."""
    xs, ys, raw = trace(np.ascontiguousarray(ids, np.int32))
    points = []
    chains = []
    total = 0
    for first, count, a, b, closed in raw:
        cx, cy = xs[first:first + count].astype(np.float64), ys[first:first + count].astype(np.float64)
        radius = WATER_RADIUS if water[a] and water[b] else LAND_RADIUS
        if closed:
            # A loop keeps its first point as a fixed seam so the polyline closes exactly.
            cx, cy = np.append(cx, cx[0]), np.append(cy, cy[0])
            sx, sy = _smooth(cx[:-1], cy[:-1], True, radius, PASSES)
            sx, sy = np.append(sx, sx[0]), np.append(sy, sy[0])
        else:
            sx, sy = _smooth(cx, cy, False, radius, PASSES)
        keep = _simplify(sx, sy, TOLERANCE)
        chain = np.stack([sx[keep], sy[keep]], axis=1).astype(np.float32)
        points.append(chain)
        chains.append((total, len(chain), int(a) | (int(b) << 16), CLOSED if closed else 0))
        total += len(chain)
    return np.concatenate(points), np.array(chains, np.uint32)


def main():
    import time
    from PIL import Image, ImageDraw
    sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
    from sources import province_map
    started = time.time()
    ids, provinces = province_map()
    water = np.zeros(ids.max() + 1, bool)
    for pid, row in provinces.items():
        water[pid] = row['type'] in ('sea', 'lake')
    points, chains = build(ids, water)
    print(f'{len(chains)} chains, {len(points)} points in {time.time() - started:.1f}s')
    # Preview: a patch of the map ×6 with the pixel boundaries and the smoothed lines.
    x0, y0, size, zoom = 2150, 1500, 160, 6
    crop = ids[y0:y0 + size, x0:x0 + size]
    rng = np.random.default_rng(1)
    lut = (rng.random((ids.max() + 1, 3)) * 120 + 120).astype(np.uint8)
    image = Image.fromarray(lut[crop]).resize((size * zoom, size * zoom), Image.NEAREST)
    draw = ImageDraw.Draw(image)
    for first, count, pair, flags in chains:
        p = points[first:first + count]
        if (p[:, 0].max() < x0 or p[:, 0].min() > x0 + size or p[:, 1].max() < y0 or p[:, 1].min() > y0 + size):
            continue
        a, b = pair & 0xFFFF, pair >> 16
        color = (40, 90, 200) if water[a] and water[b] else (20, 20, 20)
        draw.line([((x - x0) * zoom, (y - y0) * zoom) for x, y in p], fill=color, width=2)
    out = ROOT / 'build' / 'map' / 'borders_preview.png'
    image.save(out)
    print(out)


if __name__ == '__main__':
    main()
