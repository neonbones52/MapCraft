"""Cut the map into regions and provinces on the block grid (one pixel per block) and name everything.

Reads the map sources in the repository root through the other mapgen modules: heightmap.png (heightmap.py),
wasteland.png and borders.png (border_lines.py), biome.png (biomes.py), rivers.png (river_paths.py).

- Lands are the user's land lines (border_lines.py). A land is cut into regions of about REGION_PROVINCES provinces; region
  lines, where drawn, cut it first. A piece too small to be a region (an island) joins the nearest region of its land.
- The passable land holds LAND_PROVINCES provinces, given to regions by area. A piece cut off by province lines is one
  fixed province; the rest of every region is cut automatically. An island is never part of a province elsewhere: a
  piece of a province on another landmass becomes a province of its own (on top of LAND_PROVINCES).
- Both cuts are geodesic Voronoi diagrams relaxed with Lloyd iterations. A step costs 1 + noise, RIVER_COST more into a
  river block and STEP_COST more per height step, so borders run along rivers and ridges. Cells never leave their zone.
- Every connected piece of wasteland is one province; every lake of at least MIN_LAKE blocks is one; decorative lakes
  join the province around them. The sea is cut into coastal and open sea provinces.
- Names come from names.py, after the biome, height and coast of each place.

    python tools/mapgen/generate_provinces.py   # -> build/provinces/: provinces.png/.csv, regions.png/.csv,
                                                #    adjacency.csv, preview.png, report.txt
The output is copied to the root by hand; from then on the provinces are frozen (edit_provinces.py).
"""
import csv
import heapq
import math
import sys

import numpy as np
from numba import njit
from PIL import Image
from scipy import ndimage
from scipy.cluster.vq import kmeans2

from biomes import read_biomes
from border_lines import assign_lands, fixed_cells, passable_land, read_borders
from heightmap import ROOT, read_blocks
from names import Namer
from palette import distinct_colors
from river_paths import read_lines, river_mask, trace
from validate_land import FOUR, LAKE, MIN_LAKE, SEA

LAND_PROVINCES = 850          # provinces on the passable land, fixed ones included
REGION_PROVINCES = 8          # provinces per automatic region
SATELLITE_SHARE = 0.35        # pieces smaller than this share of a cell join their neighbours instead of getting one
LLOYD_STEPS = 6
NOISE_STRENGTH = 1.0          # step cost = 1 + NOISE_STRENGTH * fractal noise in [0, 1]
RIVER_COST = 10.0             # extra cost of stepping into a river block
STEP_COST = 3.0               # extra cost per height step climbed or descended
COAST_BAND = 28               # blocks of sea along the coast that form the coastal zone
COAST_PROVINCE = 1400         # blocks per coastal sea province
OPEN_PROVINCE = 5600          # blocks per open sea province
MIN_OPEN_RADIUS = 18          # blocks; open sea pieces narrower than twice this join the coastal zone
MIN_SEA_PROVINCE = 190        # blocks; smaller sea provinces join the sea neighbour they share most border with
SEA_NECK = 2                  # blocks; parts of a sea province narrower than 2 * SEA_NECK + 1 go to a sea neighbour
LAKE_DENSITY = 1.5            # province density on the shore of a lake province, times the biome's density
LAKE_REACH = 40               # blocks from the lake where its extra density fades out
SEED = 20261114

SQRT2 = math.sqrt(2.0)
LAND, WASTELAND, SEA_KIND, LAKE_KIND = 1, 2, 3, 4
KIND_NAMES = {LAND: 'land', WASTELAND: 'wasteland', SEA_KIND: 'sea', LAKE_KIND: 'lake'}


def fractal_noise(shape, rng, scales=(40, 16, 6, 2), weights=(1.0, 0.6, 0.35, 0.15)):
    total = np.zeros(shape, np.float32)
    for scale, weight in zip(scales, weights):
        grid = rng.random((shape[0] // scale + 3, shape[1] // scale + 3)).astype(np.float32)
        layer = ndimage.zoom(grid, scale, order=3)[:shape[0], :shape[1]]
        total += weight * layer
    total -= total.min()
    return total / total.max()


@njit(cache=True)
def geodesic_voronoi(zone, cost, height, seeds):
    """Multi-source Dijkstra; cells never cross zone borders or cut diagonal corners. A step costs the mean of the two
    blocks' costs times its length, plus STEP_COST per height step between them."""
    rows, cols = zone.shape
    flat_zone = zone.ravel()
    flat_cost = cost.ravel()
    flat_height = height.ravel()
    dist = np.full(rows * cols, np.inf, np.float64)
    label = np.zeros(rows * cols, np.int32)
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
        y = p // cols
        x = p - y * cols
        z = flat_zone[p]
        for k in range(8):
            yy = y + dys[k]
            xx = x + dxs[k]
            if yy < 0 or yy >= rows or xx < 0 or xx >= cols:
                continue
            q = yy * cols + xx
            if flat_zone[q] != z:
                continue
            step = 1.0
            if dys[k] != 0 and dxs[k] != 0:
                if flat_zone[y * cols + xx] != z or flat_zone[yy * cols + x] != z:
                    continue
                step = SQRT2
            nd = d + step * 0.5 * (flat_cost[p] + flat_cost[q]) + STEP_COST * abs(flat_height[p] - flat_height[q])
            if nd < dist[q]:
                dist[q] = nd
                label[q] = label[p]
                heapq.heappush(heap, (nd, q))
    return label.reshape(rows, cols)


def initial_seeds(mask, weight, count, rng):
    """k-means on block coordinates of one piece drawn in proportion to their weight, snapped to blocks of the
    piece: dense places get more seeds."""
    ys, xs = np.nonzero(mask)
    points = np.column_stack([ys, xs]).astype(np.float64)
    p = weight[ys, xs].astype(np.float64)
    points = points[rng.choice(len(points), min(len(points), 20000), replace=False, p=p / p.sum())]
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


def relax(zone, cost, height, weight, seeds, steps):
    """Lloyd iterations: move every seed to the block of its cell nearest to the cell's weighted centroid, so cells
    shrink where the weight is high."""
    cols = zone.shape[1]
    seeds = np.array(seeds, np.int64)
    flat_weight = weight.ravel().astype(np.float64)
    for step in range(steps + 1):
        labels = geodesic_voronoi(zone, cost, height, seeds)
        if step == steps:
            return labels
        flat = labels.ravel()
        inside = np.nonzero(flat)[0]
        owner = flat[inside]
        w = flat_weight[inside]
        ys, xs = inside // cols, inside % cols
        count = np.bincount(owner, w, minlength=len(seeds) + 1)
        cy = np.bincount(owner, ys * w, minlength=len(seeds) + 1) / np.maximum(count, 1e-9)
        cx = np.bincount(owner, xs * w, minlength=len(seeds) + 1) / np.maximum(count, 1e-9)
        d2 = (ys - cy[owner]) ** 2 + (xs - cx[owner]) ** 2
        order = np.lexsort((d2, owner))
        first = np.ones(len(order), bool)
        first[1:] = owner[order][1:] != owner[order][:-1]
        for label, block in zip(owner[order][first], inside[order][first]):
            seeds[label - 1] = block


def allocate(areas, total):
    """Split `total` over pieces by area (largest remainder), at least one each."""
    total = max(total, len(areas))
    share = np.array(areas, np.float64) / sum(areas) * total
    result = np.maximum(np.floor(share).astype(int), 1)
    while result.sum() < total:
        result[np.argmax(share - result)] += 1
    while result.sum() > total:
        candidates = np.where(result > 1, share - result, np.inf)
        result[np.argmin(candidates)] -= 1
    return result


def cut(zone, counts, cost, height, weight, rng):
    """Cut every zone into counts[zone] cells, smaller where the weight is higher. A zone's piece too small for a
    cell of its own gets no seed and joins the nearest cell of its zone whole. -> (cell label per block, zone of
    every cell)."""
    seeds, owner = [], []
    for zone_id, box in enumerate(ndimage.find_objects(zone), 1):
        if box is None or not counts.get(zone_id):
            continue
        mask = zone[box] == zone_id
        local_weight = weight[box]
        wanted = counts[zone_id]
        parts, count = ndimage.label(mask, FOUR)
        areas = ndimage.sum(local_weight, parts, range(1, count + 1))
        blocks = np.bincount(parts.ravel())[1:]
        total = float(np.sum(areas))
        major = [i + 1 for i in range(count) if areas[i] >= SATELLITE_SHARE * total / wanted] or \
                [int(np.argmax(areas)) + 1]
        for part, number in zip(major, allocate([areas[i - 1] for i in major], wanted)):
            for y, x in initial_seeds(parts == part, local_weight, min(number, int(blocks[part - 1])), rng):
                seeds.append((y + box[0].start) * zone.shape[1] + x + box[1].start)
                owner.append(zone_id)
    labels = relax(zone, cost, height, weight, seeds, LLOYD_STEPS)
    join_whole(labels, zone)
    repair_fragments(labels, zone)
    return labels, np.array([0] + owner, np.int32)


def separate_islands(labels, landmass, which=None):
    """An island is never part of a province elsewhere: every piece of a cell lying on another landmass than the cell's
    largest piece becomes a cell of its own, numbered after the last label. Blocks off any landmass (landmass 0: a
    decorative lake) stay with their cell. `which` limits it to some labels. -> [(new label, label it came from)]."""
    added = []
    next_label = int(labels.max())
    for label, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None or (which is not None and label not in which):
            continue
        own = labels[box] == label
        masses = np.unique(landmass[box][own & (landmass[box] > 0)])
        if len(masses) < 2:
            continue
        sizes = {m: int((landmass[box][own] == m).sum()) for m in masses}
        keep = max(sizes, key=sizes.get)
        for mass in sorted(masses):
            if mass == keep:
                continue
            next_label += 1
            labels[box][own & (landmass[box] == mass)] = next_label
            added.append((next_label, label))
    return added


def join_whole(labels, zone):
    """Every connected piece of a zone left without a label joins, whole, the label nearest to it in its zone."""
    todo, _ = ndimage.label((zone > 0) & (labels == 0), FOUR)
    for piece, box in enumerate(ndimage.find_objects(todo), 1):
        mine = todo == piece
        zone_id = zone[mine][0]
        known = (zone == zone_id) & (labels > 0)
        if not known.any():
            continue
        distance, (iy, ix) = ndimage.distance_transform_edt(~known, return_indices=True)
        ys, xs = np.nonzero(mine)
        k = int(np.argmin(distance[ys, xs]))
        labels[mine] = labels[iy[ys[k], xs[k]], ix[ys[k], xs[k]]]


def repair_fragments(labels, zone):
    """A detached piece of a cell joins the neighbour of the same zone it shares most border with; islets stay."""
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
            same = zone[grown][ring] == zone[grown][piece][0]
            neighbours = neighbours[(neighbours > 0) & (neighbours != label) & same]
            if len(neighbours):
                labels[grown][piece] = np.bincount(neighbours).argmax()


def merge_small(labels, zone, minimum):
    """Pieces of `zone` (labels) smaller than `minimum` join, whole, the nearest other piece of the same parent:
    -> labels renumbered densely. `zone` is the parent label per block."""
    areas = np.bincount(labels.ravel())
    for label in np.flatnonzero((areas < minimum) & (areas > 0)):
        mine = labels == label
        parent = zone[mine][0]
        others = (zone == parent) & (labels > 0) & ~mine & (areas[labels] >= minimum)
        if not others.any():
            continue
        distance, (iy, ix) = ndimage.distance_transform_edt(~others, return_indices=True)
        ys, xs = np.nonzero(mine)
        k = int(np.argmin(distance[ys, xs]))
        labels[mine] = labels[iy[ys[k], xs[k]], ix[ys[k], xs[k]]]
    used = np.unique(labels[labels > 0])
    remap = np.zeros(labels.max() + 1, np.int32)
    remap[used] = np.arange(1, len(used) + 1)
    return remap[labels]


def trim_necks(labels, radius):
    """Parts of a cell narrower than a disc of `radius` (necks, panhandles) go to the neighbouring cell they share
    most border with; a part with no other neighbour (an inlet only this cell reaches) stays. -> blocks moved."""
    disc = np.hypot(*np.mgrid[-radius:radius + 1, -radius:radius + 1]) <= radius + 0.5
    moved = 0
    for label, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None:
            continue
        grown = tuple(slice(max(s.start - radius - 1, 0), s.stop + radius + 1) for s in box)
        own = labels[grown] == label
        body = ndimage.binary_opening(own, disc)
        if body.sum() < own.sum() / 2:
            continue                                  # a small or thin cell as a whole: leave it
        parts, count = ndimage.label(own & ~body, FOUR)
        for part in range(1, count + 1):
            piece = parts == part
            ring = ndimage.binary_dilation(piece, FOUR) & ~piece
            neighbours = labels[grown][ring]
            neighbours = neighbours[(neighbours > 0) & (neighbours != label)]
            if len(neighbours):
                labels[grown][piece] = np.bincount(neighbours).argmax()
                moved += int(piece.sum())
    return moved


def plan_sea(classes, noise, rng, cost, height, log):
    """Coastal and open sea zones, then sea provinces in them."""
    sea = classes == SEA
    distance = ndimage.distance_transform_edt(sea)
    coastal = sea & (distance + 10 * (noise - 0.5) <= COAST_BAND)
    open_parts, open_count = ndimage.label(sea & ~coastal, FOUR)
    radius = ndimage.maximum(ndimage.distance_transform_edt(open_parts > 0), open_parts, range(1, open_count + 1))
    thin = [index + 1 for index, value in enumerate(np.atleast_1d(radius)) if value < MIN_OPEN_RADIUS]
    coastal |= np.isin(open_parts, thin)
    zone = np.zeros(classes.shape, np.int32)
    counts, opened = {}, {}
    next_zone = 1
    for mask_all, target, is_open in ((coastal, COAST_PROVINCE, False), (sea & ~coastal, OPEN_PROVINCE, True)):
        parts, count = ndimage.label(mask_all, FOUR)
        areas = np.bincount(parts.ravel())
        for index in range(1, count + 1):
            zone[parts == index] = next_zone
            counts[next_zone] = max(1, round(areas[index] / target))
            opened[next_zone] = is_open
            next_zone += 1
    labels, owner = cut(zone, counts, np.ones_like(cost), np.zeros_like(height), np.ones_like(cost), rng)
    moved = trim_necks(labels, SEA_NECK)
    labels = merge_small(labels, np.where(sea, 1, 0), MIN_SEA_PROVINCE)
    log.append(f'sea: coastal zone {int(coastal.sum())} blocks, open {int((sea & ~coastal).sum())} blocks, '
               f'{labels.max()} provinces; {moved} blocks of narrow necks moved to neighbours')
    open_sea = np.zeros(labels.max() + 1, bool)
    for label in range(1, labels.max() + 1):
        open_sea[label] = opened[int(np.bincount(zone[labels == label]).argmax())]
    return labels, open_sea


def density(biomes, biome_rows, classes):
    """Province density per block: the biome's `density` (data/common/biomes.csv), raised towards the shore of a lake
    province up to LAKE_DENSITY times, fading out over LAKE_REACH blocks. Cells are smaller where it is higher."""
    per_biome = np.array([1.0] + [float(row.get('density') or 1.0) for row in biome_rows], np.float32)
    weight = per_biome[biomes]
    lakes, count = ndimage.label(classes == LAKE, FOUR)
    sizes = np.bincount(lakes.ravel())
    big = np.isin(lakes, [i for i in range(1, count + 1) if sizes[i] >= MIN_LAKE])
    if big.any():
        reach = np.clip(1.0 - ndimage.distance_transform_edt(~big) / LAKE_REACH, 0.0, 1.0)
        weight = weight * (1.0 + (LAKE_DENSITY - 1.0) * reach).astype(np.float32)
    return np.where(weight > 0, weight, 1.0).astype(np.float32)


def dominant(values, labels, count):
    """Most common value per label 1..count."""
    out = np.zeros(count + 1, np.int64)
    keys = labels.ravel().astype(np.int64) * 256 + values.ravel()
    keys = keys[labels.ravel() > 0]
    found, numbers = np.unique(keys, return_counts=True)
    best = np.zeros(count + 1, np.int64)
    for key, number in zip(found, numbers):
        label = key // 256
        if number > best[label]:
            best[label], out[label] = number, key % 256
    return out


def adjacency(labels):
    pairs = {}
    for a, b in ((labels[:, :-1], labels[:, 1:]), (labels[:-1, :], labels[1:, :])):
        border = (a != b) & (a > 0) & (b > 0)
        lo = np.minimum(a[border], b[border]).astype(np.int64)
        hi = np.maximum(a[border], b[border]).astype(np.int64)
        keys, counts = np.unique(lo * 100000 + hi, return_counts=True)
        for key, count in zip(keys, counts):
            pair = (int(key // 100000), int(key % 100000))
            pairs[pair] = pairs.get(pair, 0) + int(count)
    return pairs


def edges(labels):
    edge = np.zeros(labels.shape, bool)
    edge[:, 1:] |= labels[:, 1:] != labels[:, :-1]
    edge[1:, :] |= labels[1:, :] != labels[:-1, :]
    return edge


def main():
    out = ROOT / 'build' / 'provinces'
    out.mkdir(parents=True, exist_ok=True)
    log = []
    try:
        classes, steps, _ = read_blocks()
        land, passable = passable_land()
        lines, problems = read_borders()
        if problems:
            raise ValueError('borders.png: ' + '; '.join(problems))
        biomes, biome_rows = read_biomes()
        kinds_of_lines, problems = read_lines()
        if problems:
            raise ValueError('rivers.png: ' + '; '.join(problems))
    except (ValueError, FileNotFoundError) as error:
        sys.stderr.write(f'{error}\n')
        return 1
    rivers, river_problems = trace(kinds_of_lines, classes)
    if river_problems:
        log.append(f'rivers.png has {len(river_problems)} problems; run river_paths.py')
    river = river_mask(rivers, steps)
    biome_keys = [''] + [row['key'] for row in biome_rows]
    rng = np.random.default_rng(SEED)
    noise = fractal_noise(classes.shape, rng)
    cost = (1.0 + NOISE_STRENGTH * noise + RIVER_COST * river).astype(np.float32)
    height = steps.astype(np.float32)
    weight = density(biomes, biome_rows, classes)
    province_area = float(weight[passable].sum()) / LAND_PROVINCES        # weighted blocks per province

    # Lands, line regions, fixed provinces.
    lands, _, _ = assign_lands(land, passable, lines)
    used = np.unique(lands[lands > 0])
    remap = np.zeros(lands.max() + 1, np.int32)
    remap[used] = np.arange(1, len(used) + 1)
    lands = remap[lands]
    line_regions, cells, fixed = fixed_cells(passable, lines, lands)
    line_regions = merge_small(line_regions, lands, REGION_PROVINCES * province_area / 2)
    land_count = int(lands.max())

    # Regions: every line region cut into about REGION_PROVINCES provinces' worth.
    areas = np.bincount(line_regions.ravel(), weight.ravel())
    counts = {z: max(1, round(areas[z] / (REGION_PROVINCES * province_area))) for z in range(1, len(areas))}
    regions, region_zone = cut(line_regions, counts, cost, height, weight, rng)
    for cell in fixed:                                   # a fixed province lies in one region
        mine = cells == cell
        regions[mine] = np.bincount(regions[mine]).argmax()
    regions = merge_small(regions, lands, 1)
    region_count = int(regions.max())
    region_land = np.zeros(region_count + 1, np.int32)
    for region in range(1, region_count + 1):
        region_land[region] = np.bincount(lands[regions == region]).argmax()

    # Provinces on the passable land: fixed ones, the rest of every region cut by area.
    in_fixed = np.isin(cells, list(fixed)) if fixed else np.zeros(cells.shape, bool)
    free_zone = np.where(passable & ~in_fixed, regions, 0)
    free_areas = np.bincount(free_zone.ravel(), weight.ravel(), minlength=region_count + 1)[1:]
    present = [r + 1 for r in range(region_count) if free_areas[r] > 0]
    numbers = allocate([free_areas[r - 1] for r in present], LAND_PROVINCES - len(fixed))
    provinces, owner = cut(free_zone, dict(zip(present, numbers)), cost, height, weight, rng)
    province_region = list(owner[1:])
    landmass, _ = ndimage.label(land, FOUR)
    islands = separate_islands(provinces, landmass)
    province_region += [province_region[source - 1] for _, source in islands]
    free_count = int(provinces.max())
    for k, cell in enumerate(sorted(fixed), 1):
        mine = cells == cell
        provinces[mine] = free_count + k
        province_region.append(int(np.bincount(regions[mine]).argmax()))
    land_province_count = int(provinces.max())
    log.append(f'land: {land_count} lands, {region_count} regions, {land_province_count} provinces '
               f'({len(fixed)} fixed, {len(islands)} islands cut off from provinces elsewhere)')

    # Wasteland, lakes, decorative lakes, sea.
    labels = provinces.copy()
    waste = land & ~passable
    waste_labels, waste_count = ndimage.label(waste, FOUR)
    labels[waste_labels > 0] = waste_labels[waste_labels > 0] + land_province_count
    lake_parts, lake_count = ndimage.label(classes == LAKE, FOUR)
    lake_areas = np.bincount(lake_parts.ravel())
    big = [i for i in range(1, lake_count + 1) if lake_areas[i] >= MIN_LAKE]
    next_id = land_province_count + waste_count
    for k, lake in enumerate(big, 1):
        labels[lake_parts == lake] = next_id + k
    for lake in range(1, lake_count + 1):
        if lake in big:
            continue
        mine = lake_parts == lake
        ring = ndimage.binary_dilation(mine, np.ones((3, 3), bool)) & ~mine & (labels > 0)
        labels[mine] = np.bincount(labels[ring]).argmax() if ring.any() else 0
    next_id += len(big)
    sea_labels, open_sea = plan_sea(classes, noise, rng, cost, height, log)
    labels[sea_labels > 0] = sea_labels[sea_labels > 0] + next_id
    total = int(labels.max())
    kind = np.zeros(total + 1, np.int8)
    kind[1:land_province_count + 1] = LAND
    kind[land_province_count + 1:land_province_count + waste_count + 1] = WASTELAND
    kind[land_province_count + waste_count + 1:next_id + 1] = LAKE_KIND
    kind[next_id + 1:] = SEA_KIND
    region_of = np.zeros(total + 1, np.int32)
    region_of[1:land_province_count + 1] = province_region
    log.append(f'wasteland: {waste_count} provinces; lakes: {len(big)} provinces, '
               f'{lake_count - len(big)} decorative')

    # Names.
    namer = Namer(SEED)
    biome_of = dominant(biomes.astype(np.int64), labels, total)
    mean_step = np.concatenate([[0.0], ndimage.mean(steps, labels, range(1, total + 1))])
    coast = np.zeros(total + 1, bool)
    near_sea = ndimage.binary_dilation(classes == SEA, FOUR)
    coast[np.unique(labels[near_sea & (labels > 0) & (classes != SEA)])] = True
    land_biome = dominant(biomes.astype(np.int64), lands, land_count)
    land_names = [''] + [namer.land(biome_keys[land_biome[i]]) for i in range(1, land_count + 1)]
    region_biome = dominant(biomes.astype(np.int64), regions, region_count)
    region_step = np.concatenate([[0.0], ndimage.mean(steps, regions, range(1, region_count + 1))])
    region_names = [''] + [namer.region(biome_keys[region_biome[i]], region_step[i]) for i in range(1, region_count + 1)]
    names = [''] * (total + 1)
    lake_biome = {}
    for pid in range(1, total + 1):
        if kind[pid] == LAKE_KIND:
            mine = labels == pid
            ring = ndimage.binary_dilation(mine, FOUR) & ~mine & (biomes > 0)
            lake_biome[pid] = int(np.bincount(biomes[ring]).argmax()) if ring.any() else 1
    for pid in range(1, total + 1):
        if kind[pid] == LAND:
            names[pid] = namer.province(biome_keys[biome_of[pid]], mean_step[pid], coast[pid])
        elif kind[pid] == WASTELAND:
            names[pid] = namer.wasteland(biome_keys[biome_of[pid]])
        elif kind[pid] == LAKE_KIND:
            names[pid] = namer.lake(biome_keys[lake_biome[pid]])
        else:
            names[pid] = namer.sea(bool(open_sea[pid - next_id]))

    # Files.
    colors = distinct_colors(total, seed=11)
    lut = np.array([(0, 0, 0)] + colors, np.uint8)
    Image.fromarray(lut[labels]).save(out / 'provinces.png', optimize=True)
    region_colors = distinct_colors(region_count, seed=3)
    region_lut = np.array([(0, 0, 0)] + region_colors, np.uint8)
    Image.fromarray(region_lut[regions]).save(out / 'regions.png', optimize=True)
    with open(out / 'regions.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file, lineterminator='\n')
        writer.writerow(['id', 'color', 'name', 'parent', 'provinces'])
        for region in range(1, region_count + 1):
            writer.writerow([region, '#%02X%02X%02X' % region_colors[region - 1], region_names[region],
                             land_names[region_land[region]], int((region_of == region).sum())])
    areas = np.bincount(labels.ravel(), minlength=total + 1)
    centers = ndimage.center_of_mass(labels > 0, labels, range(1, total + 1))
    boxes = ndimage.find_objects(labels)
    pieces = [0] + [ndimage.label(labels[boxes[pid - 1]] == pid, FOUR)[1] for pid in range(1, total + 1)]
    with open(out / 'provinces.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file, lineterminator='\n')
        writer.writerow(['id', 'color', 'type', 'region', 'region_name', 'parent', 'name', 'area', 'x', 'y', 'pieces',
                         'biome', 'height'])
        for pid in range(1, total + 1):
            region = region_of[pid]
            cy, cx = centers[pid - 1]
            land_side = kind[pid] in (LAND, WASTELAND)
            writer.writerow([pid, '#%02X%02X%02X' % colors[pid - 1], KIND_NAMES[kind[pid]], region or '',
                             region_names[region] if region else '', land_names[region_land[region]] if region else '',
                             names[pid], int(areas[pid]), round(cx), round(cy), pieces[pid],
                             biome_keys[biome_of[pid]] if land_side else '', round(mean_step[pid]) if land_side else ''])
    pairs = adjacency(labels)
    with open(out / 'adjacency.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file, lineterminator='\n')
        writer.writerow(['a', 'b', 'kind', 'border'])
        for (a, b), length in sorted(pairs.items()):
            writer.writerow([a, b, KIND_NAMES[kind[a]] + '-' + KIND_NAMES[kind[b]], length])

    # Preview, 2x: provinces muted over water, province borders dark, region borders red, land borders black.
    tint = lut.astype(np.float32)
    water = (kind == SEA_KIND) | (kind == LAKE_KIND)
    tint[water] = tint[water] * 0.25 + np.array([30, 70, 140]) * 0.75
    tint[kind == WASTELAND] = (110, 110, 110)
    scale = 2
    big_labels = labels.repeat(scale, 0).repeat(scale, 1)
    image = tint[big_labels]
    image[edges(big_labels)] = (40, 40, 40)
    region_map = region_of[labels]
    land_map = region_land[region_map].repeat(scale, 0).repeat(scale, 1)
    region_map = region_map.repeat(scale, 0).repeat(scale, 1)
    image[edges(region_map) & (region_map > 0)] = (200, 30, 30)
    image[edges(land_map) & (land_map > 0)] = (0, 0, 0)
    Image.fromarray(image.astype(np.uint8)).save(out / 'preview.png', optimize=True)

    land_areas = areas[1:land_province_count + 1]
    sea_areas = areas[next_id + 1:]
    log.append(f'total {total}: land {land_province_count}, wasteland {waste_count}, lakes {len(big)}, '
               f'sea {total - next_id}')
    log.append(f'land province blocks: min {land_areas.min()} median {int(np.median(land_areas))} max {land_areas.max()}')
    log.append(f'sea province blocks: min {sea_areas.min()} median {int(np.median(sea_areas))} max {sea_areas.max()}')
    region_sizes = np.bincount(region_of[1:land_province_count + 1], minlength=region_count + 1)[1:]
    log.append(f'provinces per region: min {region_sizes.min()} median {int(np.median(region_sizes))} '
               f'max {region_sizes.max()}')
    log.append(f'multi-piece land provinces: {sum(1 for pid in range(1, land_province_count + 1) if pieces[pid] > 1)}')
    log.append(f'adjacencies: {len(pairs)}')
    text = '\n'.join(log)
    (out / 'report.txt').write_text(text + '\n', encoding='utf-8', newline='\n')
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
