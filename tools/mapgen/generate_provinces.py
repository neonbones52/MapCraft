"""Cut regions and sea into provinces: noisy geodesic Voronoi cells relaxed with Lloyd iterations."""
import argparse
import csv
import heapq
import math
from pathlib import Path

import numpy as np
from numba import njit
from PIL import Image, ImageDraw
from scipy import ndimage
from scipy.cluster.vq import kmeans2

from import_lineart import distinct_colors
from validate_land import FOUR, LAKE, LAND, PALETTE, SEA

MULTIPLIER = 3                # provinces per region = csv value x MULTIPLIER
MIN_LAND_PROVINCE = 4000      # px; caps the count for small regions and islands
SATELLITE_SHARE = 0.35        # pieces smaller than this share of a province join the nearest province
MIN_LAKE_PROVINCE = 2500      # px; smaller lakes are decorative water inside a land province
COAST_BAND = 110              # px of sea along the coast that forms the coastal zone
COAST_PROVINCE = 22000        # px per coastal sea province
OPEN_PROVINCE = 90000         # px per open sea province
MIN_OPEN_RADIUS = 70          # px; open sea pieces narrower than twice this join the coastal zone
MIN_SEA_PROVINCE = 3000       # px; smaller sea provinces join the sea neighbour with the longest shared border
LLOYD_STEPS = 6
NOISE_STRENGTH = 1.6          # path cost = 1 + NOISE_STRENGTH * fractal noise in [0, 1]
SEED = 20260927
WARP = ((110, 22.0), (40, 8.0), (12, 2.5))  # (noise scale px, displacement px): wiggles province borders

SQRT2 = math.sqrt(2.0)


def read_classes(path):
    rgb = np.asarray(Image.open(path).convert('RGB')).astype(np.uint32)
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    classes = np.full(key.shape, 255, np.uint8)
    for color, value in PALETTE.items():
        classes[key == ((color[0] << 16) | (color[1] << 8) | color[2])] = value
    if (classes == 255).any():
        raise ValueError(f'{path}: pixels outside the palette')
    return classes


def read_regions(png, table):
    rgb = np.asarray(Image.open(png).convert('RGB')).astype(np.uint32)
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    with open(table, encoding='utf-8-sig') as file:
        rows = list(csv.DictReader(file))
    regions = np.zeros(key.shape, np.int32)
    for row in rows:
        regions[key == int(row['color'].strip().lstrip('#'), 16)] = int(row['id'])
    return regions, {int(row['id']): row for row in rows}


def read_seeds(path, shape):
    if not path.exists():
        return []
    rgba = np.asarray(Image.open(path).convert('RGBA'))
    if rgba.shape[:2] != shape:
        raise ValueError(f'{path}: size differs from land.png')
    # Red and opaque; a transparent GIMP layer or a black background both mean "no seed".
    dots = (rgba[..., 0] >= 200) & (rgba[..., 1] <= 60) & (rgba[..., 2] <= 60) & (rgba[..., 3] >= 128)
    labels, count = ndimage.label(dots, np.ones((3, 3), bool))
    centers = ndimage.center_of_mass(dots, labels, range(1, count + 1))
    return [(int(round(y)), int(round(x))) for y, x in centers]


def fractal_noise(shape, rng, scales=(160, 64, 24, 8), weights=(1.0, 0.6, 0.35, 0.15)):
    total = np.zeros(shape, np.float32)
    for scale, weight in zip(scales, weights):
        grid = rng.random((shape[0] // scale + 3, shape[1] // scale + 3)).astype(np.float32)
        layer = ndimage.zoom(grid, scale, order=3)[:shape[0], :shape[1]]
        total += weight * layer
    total -= total.min()
    return total / total.max()


@njit(cache=True)
def geodesic_voronoi(zone, cost, seeds):
    """Multi-source Dijkstra; cells never cross zone borders or cut diagonal corners."""
    height, width = zone.shape
    flat_zone = zone.ravel()
    flat_cost = cost.ravel()
    dist = np.full(height * width, np.inf, np.float64)
    label = np.zeros(height * width, np.int32)
    heap = [(0.0, np.int64(0))]
    heap.pop()
    for index in range(seeds.shape[0]):
        p = seeds[index]
        dist[p] = 0.0
        label[p] = index + 1
        heapq.heappush(heap, (0.0, p))
    dys = (-1, -1, -1, 0, 0, 1, 1, 1)
    dxs = (-1, 0, 1, -1, 1, -1, 0, 1)
    while heap:
        d, p = heapq.heappop(heap)
        if d > dist[p]:
            continue
        y = p // width
        x = p - y * width
        z = flat_zone[p]
        for k in range(8):
            yy = y + dys[k]
            xx = x + dxs[k]
            if yy < 0 or yy >= height or xx < 0 or xx >= width:
                continue
            q = yy * width + xx
            if flat_zone[q] != z:
                continue
            step = 1.0
            if dys[k] != 0 and dxs[k] != 0:
                if flat_zone[y * width + xx] != z or flat_zone[yy * width + x] != z:
                    continue
                step = SQRT2
            nd = d + step * 0.5 * (flat_cost[p] + flat_cost[q])
            if nd < dist[q]:
                dist[q] = nd
                label[q] = label[p]
                heapq.heappush(heap, (nd, q))
    return label.reshape(height, width)


def initial_seeds(mask, count, rng):
    """k-means on the pixel coordinates of one piece, snapped to pixels of the piece."""
    ys, xs = np.nonzero(mask)
    points = np.column_stack([ys, xs]).astype(np.float64)
    if len(points) > 20000:
        points = points[rng.choice(len(points), 20000, replace=False)]
    if count == 1:
        centers = points.mean(axis=0, keepdims=True)
    else:
        centers, _ = kmeans2(points, count, minit='++', seed=rng)
    chosen = []
    for cy, cx in centers:
        order = np.argsort((points[:, 0] - cy) ** 2 + (points[:, 1] - cx) ** 2)
        for index in order:
            candidate = (int(points[index, 0]), int(points[index, 1]))
            if candidate not in chosen:
                chosen.append(candidate)
                break
    return chosen


def relax(zone, cost, seeds, fixed, steps):
    """Lloyd iterations: move every free seed to the pixel of its cell nearest to the cell centroid."""
    width = zone.shape[1]
    seeds = np.array(seeds, np.int64)
    for step in range(steps + 1):
        labels = geodesic_voronoi(zone, cost, seeds)
        if step == steps:
            return labels, seeds
        flat = labels.ravel()
        inside = np.nonzero(flat)[0]
        owner = flat[inside]
        ys, xs = inside // width, inside % width
        count = np.bincount(owner, minlength=len(seeds) + 1).astype(np.float64)
        cy = np.bincount(owner, ys, minlength=len(seeds) + 1) / np.maximum(count, 1)
        cx = np.bincount(owner, xs, minlength=len(seeds) + 1) / np.maximum(count, 1)
        d2 = (ys - cy[owner]) ** 2 + (xs - cx[owner]) ** 2
        order = np.lexsort((d2, owner))
        first = np.ones(len(order), bool)
        first[1:] = owner[order][1:] != owner[order][:-1]
        best = inside[order][first]
        for label, pixel in zip(owner[order][first], best):
            if not fixed[label - 1]:
                seeds[label - 1] = pixel


def allocate(pieces_areas, total):
    """Split `total` provinces over pieces by area (largest remainder), at least one each."""
    total = max(total, len(pieces_areas))
    share = np.array(pieces_areas, np.float64) / sum(pieces_areas) * total
    result = np.maximum(np.floor(share).astype(int), 1)
    while result.sum() < total:
        result[np.argmax(share - result)] += 1
    while result.sum() > total:
        candidates = np.where(result > 1, share - result, np.inf)
        result[np.argmin(candidates)] -= 1
    return result


def plan_land(classes, regions, table, forced, rng, log):
    """Seeds for every region; satellites (islets) get no seed and join later."""
    land = (classes == LAND) | ((classes == LAKE) & ~big_lakes(classes))
    missing = land & (regions == 0)
    if missing.any():
        _, (iy, ix) = ndimage.distance_transform_edt(regions == 0, return_indices=True)
        regions = np.where(missing, regions[iy, ix], regions)
        log.append(f'{int(missing.sum())} land px without a region joined the nearest region')
    zone = np.where(land, regions, 0).astype(np.int32)
    seeds, fixed, owner = [], [], []
    boxes = ndimage.find_objects(zone)
    for region_id, box in enumerate(boxes, 1):
        if box is None:
            continue
        mask = zone[box] == region_id
        area = int(mask.sum())
        value = (table[region_id]['provinces'] or '1').strip()
        if value.endswith('!'):
            count = int(value[:-1])          # exact count, no multiplier
        else:
            count = max(1, min(int(value) * MULTIPLIER, area // MIN_LAND_PROVINCE))
        pieces, piece_count = ndimage.label(mask, FOUR)
        piece_areas = np.bincount(pieces.ravel())[1:]
        major = [i + 1 for i in range(piece_count) if piece_areas[i] >= SATELLITE_SHARE * area / count]
        region_forced = [(y - box[0].start, x - box[1].start) for y, x in forced
                         if box[0].start <= y < box[0].stop and box[1].start <= x < box[1].stop and mask[y - box[0].start, x - box[1].start]]
        split = allocate([piece_areas[i - 1] for i in major], count)
        for piece, piece_count_wanted in zip(major, split):
            piece_mask = pieces == piece
            pinned = [p for p in region_forced if piece_mask[p]]
            free = max(piece_count_wanted - len(pinned), 0)
            local = pinned + (initial_seeds(piece_mask, free, rng) if free else [])
            for index, (y, x) in enumerate(local):
                seeds.append((y + box[0].start) * zone.shape[1] + x + box[1].start)
                fixed.append(index < len(pinned))
                owner.append(region_id)
    return zone, seeds, fixed, owner


def big_lakes(classes):
    lakes, count = ndimage.label(classes == LAKE, FOUR)
    areas = np.bincount(lakes.ravel())
    return (lakes > 0) & (areas[lakes] >= MIN_LAKE_PROVINCE)


def plan_sea(classes, noise, forced, rng, log):
    sea = classes == SEA
    distance = ndimage.distance_transform_edt(sea)
    coastal = sea & (distance + 40 * (noise - 0.5) <= COAST_BAND)
    open_parts, open_count = ndimage.label(sea & ~coastal, FOUR)
    radius = ndimage.maximum(ndimage.distance_transform_edt(open_parts > 0), open_parts, range(1, open_count + 1))
    thin = [index + 1 for index, value in enumerate(radius) if value < MIN_OPEN_RADIUS]
    coastal |= np.isin(open_parts, thin)
    log.append(f'sea: {len(thin)} thin open-sea pieces moved into the coastal zone')
    zone = np.zeros(classes.shape, np.int32)
    seeds, fixed = [], []
    next_zone = 1
    for mask_all, target in ((coastal, COAST_PROVINCE), (sea & ~coastal, OPEN_PROVINCE)):
        parts, count = ndimage.label(mask_all, FOUR)
        areas = np.bincount(parts.ravel())
        for index, box in enumerate(ndimage.find_objects(parts), 1):
            if areas[index] < 200:
                continue
            mask = parts[box] == index
            zone[box][mask] = next_zone
            number = max(1, round(areas[index] / target))
            pinned = [(y - box[0].start, x - box[1].start) for y, x in forced
                      if box[0].start <= y < box[0].stop and box[1].start <= x < box[1].stop and mask[y - box[0].start, x - box[1].start]]
            free = max(number - len(pinned), 0)
            local = pinned + (initial_seeds(mask, free, rng) if free else [])
            for k, (y, x) in enumerate(local):
                seeds.append((y + box[0].start) * zone.shape[1] + x + box[1].start)
                fixed.append(k < len(pinned))
            next_zone += 1
    leftover = sea & (zone == 0)
    log.append(f'sea: {len(seeds)} seeds, coastal zone {int(coastal.sum())} px, open {int((sea & ~coastal).sum())} px, {int(leftover.sum())} px in slivers')
    return zone, seeds, fixed


def fill_unreached(labels, mask, same):
    """Unlabelled pixels in `mask` take the label of the nearest labelled pixel with the same `same` value."""
    todo = mask & (labels == 0)
    if not todo.any():
        return 0
    for value in np.unique(same[todo]):
        within = same == value
        known = within & (labels > 0)
        if not known.any():
            known = mask & (labels > 0)
        box = ndimage.find_objects(within.astype(np.int32))[0]
        _, (iy, ix) = ndimage.distance_transform_edt(~known[box], return_indices=True)
        local = labels[box]
        hole = todo[box] & within[box]
        local[hole] = local[iy[hole], ix[hole]]
    return int(todo.sum())


def warp_labels(labels, zones, rng):
    """Resample labels through a noisy displacement field; a pixel only takes labels from its own zone."""
    height, width = labels.shape
    dy = np.zeros(labels.shape, np.float32)
    dx = np.zeros(labels.shape, np.float32)
    for scale, amount in WARP:
        for field in (dy, dx):
            field += amount * (2.0 * fractal_noise(labels.shape, rng, scales=(scale,), weights=(1.0,)) - 1.0)
    ys, xs = np.indices(labels.shape)
    sy = np.clip(np.rint(ys + dy).astype(np.int32), 0, height - 1)
    sx = np.clip(np.rint(xs + dx).astype(np.int32), 0, width - 1)
    same = (zones[sy, sx] == zones) & (zones != 0)
    warped = np.where(same, labels[sy, sx], labels)
    return warped, int((warped != labels).sum())


def repair_fragments(labels, kind_of):
    """A detached piece of a province joins its dominant neighbour of the same kind; islets stay."""
    moved = 0
    for label, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None:
            continue
        grown = tuple(slice(max(s.start - 1, 0), s.stop + 1) for s in box)
        own = labels[grown] == label
        parts, count = ndimage.label(own, FOUR)
        if count < 2:
            continue
        areas = np.bincount(parts.ravel())
        keep = np.argmax(areas[1:]) + 1
        for part in range(1, count + 1):
            if part == keep:
                continue
            piece = parts == part
            ring = ndimage.binary_dilation(piece, FOUR) & ~piece
            neighbours = labels[grown][ring]
            neighbours = neighbours[(neighbours > 0) & (neighbours != label)]
            neighbours = neighbours[kind_of[neighbours] == kind_of[label]]
            if len(neighbours):
                labels[grown][piece] = np.bincount(neighbours).argmax()
                moved += int(piece.sum())
    return moved


def merge_small_sea(labels, kind_of, log):
    """Fold tiny sea provinces into a sea neighbour, then renumber ids densely."""
    areas = np.bincount(labels.ravel(), minlength=len(kind_of))
    merged = 0
    for label in np.nonzero((kind_of == 2) & (areas < MIN_SEA_PROVINCE) & (areas > 0))[0]:
        box = ndimage.find_objects((labels == label).astype(np.int32))[0]
        grown = tuple(slice(max(s.start - 1, 0), s.stop + 1) for s in box)
        own = labels[grown] == label
        ring = ndimage.binary_dilation(own, FOUR) & ~own
        neighbours = labels[grown][ring]
        neighbours = neighbours[kind_of[neighbours] == 2]
        if len(neighbours):
            labels[grown][own] = np.bincount(neighbours).argmax()
            merged += 1
    used = np.unique(labels[labels > 0])
    remap = np.zeros(len(kind_of), np.int32)
    remap[used] = np.arange(1, len(used) + 1)
    log.append(f'sea: {merged} provinces under {MIN_SEA_PROVINCE} px merged into neighbours')
    return remap[labels], used


def adjacency(labels):
    pairs = {}
    for a, b in ((labels[:, :-1], labels[:, 1:]), (labels[:-1, :], labels[1:, :])):
        border = (a != b) & (a > 0) & (b > 0)
        lo = np.minimum(a[border], b[border]).astype(np.int64)
        hi = np.maximum(a[border], b[border]).astype(np.int64)
        keys, counts = np.unique(lo * 100000 + hi, return_counts=True)
        for key, count in zip(keys, counts):
            pairs[(int(key // 100000), int(key % 100000))] = pairs.get((int(key // 100000), int(key % 100000)), 0) + int(count)
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('.'))
    parser.add_argument('--out', type=Path, default=Path('build/provinces'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    log = []

    classes = read_classes(args.root / 'land.png')
    regions, table = read_regions(args.root / 'regions.png', args.root / 'regions.csv')
    forced = read_seeds(args.root / 'seeds.png', classes.shape)
    if forced:
        log.append(f'seeds.png: {len(forced)} fixed province centres')
    noise = fractal_noise(classes.shape, rng)
    cost = (1.0 + NOISE_STRENGTH * noise).astype(np.float32)

    land_zone, land_seeds, land_fixed, land_owner = plan_land(classes, regions, table, forced, rng, log)
    land_labels, _ = relax(land_zone, cost, land_seeds, land_fixed, LLOYD_STEPS)
    reached = fill_unreached(land_labels, land_zone > 0, land_zone)
    log.append(f'land: {len(land_seeds)} provinces, {reached} px of islets joined the nearest province of their region')

    sea_zone, sea_seeds, sea_fixed = plan_sea(classes, noise, forced, rng, log)
    sea_labels, _ = relax(sea_zone, cost, sea_seeds, sea_fixed, LLOYD_STEPS)
    fill_unreached(sea_labels, classes == SEA, np.ones_like(sea_zone))

    lakes, lake_count = ndimage.label(big_lakes(classes), FOUR)

    labels = land_labels.copy()
    land_count = len(land_seeds)
    labels[sea_labels > 0] = sea_labels[sea_labels > 0] + land_count
    labels[lakes > 0] = lakes[lakes > 0] + land_count + len(sea_seeds)
    zones = np.where(land_zone > 0, land_zone, 0)
    zones[classes == SEA] = 1 << 20
    labels, warped = warp_labels(labels, zones, rng)
    log.append(f'warp: {warped} px changed owner')
    kind_of = np.array([0] + [1] * land_count + [2] * len(sea_seeds) + [3] * lake_count, np.int8)
    moved = repair_fragments(labels, kind_of)
    log.append(f'fragments: {moved} px reassigned to neighbours')

    kinds = ['land'] * land_count + ['sea'] * len(sea_seeds) + ['lake'] * lake_count
    owners = land_owner + [0] * (len(sea_seeds) + lake_count)
    labels, used = merge_small_sea(labels, kind_of, log)
    kinds = [kinds[i - 1] for i in used]
    owners = [owners[i - 1] for i in used]
    sea_count = kinds.count('sea')
    total = len(used)
    colors = distinct_colors(total, seed=11)
    lut = np.array([(0, 0, 0)] + colors, np.uint8)
    Image.fromarray(lut[labels]).save(args.out / 'provinces.png', optimize=True)

    areas = np.bincount(labels.ravel(), minlength=total + 1)
    centers = ndimage.center_of_mass(labels > 0, labels, range(1, total + 1))
    pieces = [0] * total
    for label, box in enumerate(ndimage.find_objects(labels), 1):
        if box is not None:
            pieces[label - 1] = ndimage.label(labels[box] == label, FOUR)[1]
    with open(args.out / 'provinces.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file)
        writer.writerow(['id', 'color', 'type', 'region', 'region_name', 'parent', 'name', 'area', 'x', 'y', 'pieces'])
        for index in range(total):
            region = owners[index]
            row = table.get(region, {})
            cy, cx = centers[index]
            writer.writerow([index + 1, '#%02X%02X%02X' % colors[index], kinds[index], region or '', row.get('name', ''), row.get('parent', ''),
                             '', int(areas[index + 1]), round(cx), round(cy), pieces[index]])

    pairs = adjacency(labels)
    with open(args.out / 'adjacency.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file)
        writer.writerow(['a', 'b', 'kind', 'border_px'])
        for (a, b), length in sorted(pairs.items()):
            writer.writerow([a, b, kinds[a - 1] + '-' + kinds[b - 1], length])

    # Preview: province colours muted, sea blue shades, province borders dark, region borders red.
    tint = lut.astype(np.float32)
    water = np.array([k != 'land' for k in kinds])
    tint[1:][water] = tint[1:][water] * 0.25 + np.array([30, 70, 140]) * 0.75
    image = tint[labels]
    edge_h = np.zeros(labels.shape, bool)
    edge_h[:, 1:] |= labels[:, 1:] != labels[:, :-1]
    edge_h[1:, :] |= labels[1:, :] != labels[:-1, :]
    image[edge_h] = (25, 25, 25)
    region_map = np.where(labels > 0, np.array([0] + owners, np.int32)[labels], 0)
    region_edge = np.zeros(labels.shape, bool)
    region_edge[:, 1:] |= (region_map[:, 1:] != region_map[:, :-1]) & (region_map[:, 1:] > 0) & (region_map[:, :-1] > 0)
    region_edge[1:, :] |= (region_map[1:, :] != region_map[:-1, :]) & (region_map[1:, :] > 0) & (region_map[:-1, :] > 0)
    image[ndimage.binary_dilation(region_edge)] = (200, 30, 30)
    Image.fromarray(image.astype(np.uint8)).save(args.out / 'preview_full.png', optimize=True)

    land_areas = areas[1:land_count + 1]
    sea_areas = areas[land_count + 1:land_count + sea_count + 1]
    log.append(f'total {total}: land {land_count}, sea {sea_count}, lakes {lake_count}')
    log.append(f'land area px min {land_areas.min()} median {int(np.median(land_areas))} max {land_areas.max()}')
    log.append(f'sea area px min {sea_areas.min()} median {int(np.median(sea_areas))} max {sea_areas.max()}')
    log.append(f'multi-piece provinces: land {sum(1 for i in range(land_count) if pieces[i] > 1)}, sea {sum(1 for i in range(land_count, land_count + sea_count) if pieces[i] > 1)}')
    log.append(f'adjacencies: {len(pairs)}')
    text = '\n'.join(log)
    (args.out / 'report.txt').write_text(text + '\n', encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
