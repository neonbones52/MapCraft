"""Hand edits of the frozen province map, keeping every derived file consistent.

  python tools/mapgen/edit_provinces.py split-islands            # small islands shared by several provinces
  python tools/mapgen/edit_provinces.py give-island X Y NAME     # the whole island at block (X, Y) -> province NAME
  python tools/mapgen/edit_provinces.py islands-apart            # islands that are part of a province elsewhere
                                                                 #   become provinces of their own
  python tools/mapgen/edit_provinces.py sync-lakes               # lake provinces follow lakes shrunk or removed in land.png

islands-apart: every piece of a land province on another landmass than its largest piece becomes a new land province
with the next free id, the same region, a new name (names.py) and a new colour; no id changes.

Everything is on the block grid (one pixel per block). An edit rewrites provinces.png, regions.png (moved blocks take
the region of their new province), provinces.csv (area, centroid, pieces) and adjacency.csv. Afterwards run
positions.py, bake.py.

sync-lakes: where land.png has less lake than a lake province, the province keeps its largest piece of lake; the dried
bed and the smaller pieces cut off from it (decorative lakes from now on) are shared among the land
provinces around them (noisy Voronoi from their shores, so the new borders wiggle like generated ones). A lake
province with no lake left is removed and the provinces after it are renumbered (ids stay 1..N); names, colours and
the rest of their rows are kept.
"""
import csv
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from sources import province_blocks  # noqa: E402
from biomes import read_biomes  # noqa: E402
from generate_provinces import FOUR, SEED as GENERATOR_SEED, adjacency, fractal_noise, separate_islands  # noqa: E402
from heightmap import read_blocks  # noqa: E402
from names import Namer  # noqa: E402
from palette import distinct_colors  # noqa: E402
from skimage.segmentation import watershed  # noqa: E402
from validate_land import LAKE, LAND, SEA  # noqa: E402

SMALL_ISLAND = 1250     # blocks
BED_NOISE = 3.5         # blocks: how far the borders across a dried lake bed wander from the straight Voronoi line
SEED = 20260930


def islands(classes):
    return ndimage.label(classes != SEA, FOUR)


def split_islands(ids, provinces, classes):
    labels, _ = islands(classes)
    for index, box in enumerate(ndimage.find_objects(labels), 1):
        mask = labels[box] == index
        if mask.sum() > SMALL_ISLAND:
            continue
        owners, counts = np.unique(ids[box][mask], return_counts=True)
        if len(owners) > 1:
            ys, xs = np.nonzero(mask)
            parts = ', '.join(f'{provinces[o]["name"]} {n} blocks' for o, n in zip(owners, counts))
            print(f'island at ({xs[0] + box[1].start}, {ys[0] + box[0].start}), {mask.sum()} blocks: {parts}')


def write(ids, old_ids, provinces):
    lut = np.zeros((ids.max() + 1, 3), np.uint8)
    for pid, row in provinces.items():
        lut[pid] = [int(row['color'][i:i + 2], 16) for i in (1, 3, 5)]
    Image.fromarray(lut[ids]).save(ROOT / 'provinces.png', optimize=True)

    with open(ROOT / 'regions.csv', encoding='utf-8-sig') as file:
        region_color = {row['id']: [int(row['color'].strip().lstrip('#')[i:i + 2], 16) for i in (0, 2, 4)]
                        for row in csv.DictReader(file)}
    regions = np.asarray(Image.open(ROOT / 'regions.png').convert('RGB')).copy()
    changed = ids != old_ids
    for pid in np.unique(ids[changed]):
        if provinces[pid]['type'] == 'land':
            regions[changed & (ids == pid)] = region_color[provinces[pid]['region']]
    Image.fromarray(regions).save(ROOT / 'regions.png', optimize=True)

    total = ids.max()
    areas = np.bincount(ids.ravel(), minlength=total + 1)
    centers = ndimage.center_of_mass(ids > 0, ids, range(1, total + 1))
    rows = [provinces[pid] for pid in sorted(provinces)]
    for row, box in zip(rows, ndimage.find_objects(ids)):
        pid = int(row['id'])
        cy, cx = centers[pid - 1]
        row.update(area=int(areas[pid]), x=round(cx), y=round(cy), pieces=ndimage.label(ids[box] == pid, FOUR)[1])
    with open(ROOT / 'provinces.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)

    before, after = adjacency(old_ids), adjacency(ids)
    with open(ROOT / 'adjacency.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file, lineterminator='\n')
        writer.writerow(['a', 'b', 'kind', 'border'])
        for (a, b), length in sorted(after.items()):
            writer.writerow([a, b, provinces[a]['type'] + '-' + provinces[b]['type'], length])
    for a, b in sorted(set(before) ^ set(after)):
        print(f'adjacency {"new" if (a, b) in after else "lost"}: {provinces[a]["name"]} - {provinces[b]["name"]}')


def give_island(ids, provinces, classes, x, y, name):
    matches = [pid for pid, row in provinces.items() if row['name'] == name]
    if len(matches) != 1:
        sys.exit(f'{len(matches)} provinces named {name!r}')
    labels, _ = islands(classes)
    if labels[y, x] == 0:
        sys.exit(f'({x}, {y}) is sea')
    island = (labels == labels[y, x]) & (provinces_kind(ids, provinces) == 'land')
    old_ids = ids.copy()
    ids[island] = matches[0]
    for pid in np.unique(old_ids[island]):
        if pid != matches[0]:
            left = (ids == pid).sum()
            print(f'{provinces[pid]["name"]}: gave {(old_ids[island] == pid).sum()} blocks, keeps {left} blocks')
            if left == 0:
                sys.exit(f'{provinces[pid]["name"]} would vanish; nothing written')
    write(ids, old_ids, provinces)


def islands_apart(ids, provinces, classes):
    """Islands that are part of a land province elsewhere become provinces of their own (generate_provinces does the
    same when it cuts)."""
    _, steps, _ = read_blocks()
    biomes, biome_rows = read_biomes()
    landmass, _ = ndimage.label(classes == LAND, FOUR)       # land and wasteland, as generate_provinces
    old_ids = ids.copy()
    added = separate_islands(ids, landmass, {pid for pid, row in provinces.items() if row['type'] == 'land'})
    if not added:
        sys.exit('no island is part of a province elsewhere')
    namer = Namer(GENERATOR_SEED + 1)
    namer.used = {row['name'] for row in provinces.values()}
    taken = {row['color'].upper() for row in provinces.values()}
    colors = iter(c for c in distinct_colors(len(provinces) + 4 * len(added), seed=11)
                  if '#%02X%02X%02X' % c not in taken)
    for new, source in added:
        mine = ids == new
        biome = biome_rows[int(np.bincount(biomes[mine]).argmax()) - 1]['key']
        step = float(steps[mine].mean())
        row = dict(provinces[source], id=str(new), color='#%02X%02X%02X' % next(colors),
                   name=namer.province(biome, step, True), biome=biome, height=str(round(step)))
        provinces[new] = row
        y, x = np.argwhere(mine).mean(axis=0)
        print(f'{provinces[source]["name"]}: island of {int(mine.sum())} blocks at ({x:.0f}, {y:.0f}) -> '
              f'{row["name"]} (id {new})')
    write(ids, old_ids, provinces)


def sync_lakes(ids, provinces, classes):
    """Lake provinces follow the lakes of land.png. Each keeps its largest remaining piece of lake; the rest of its
    pixels (land now, or smaller pieces of lake cut off from it, decorative from now on) join the land provinces around
    them. A lake province with no lake left is removed."""
    rng = np.random.default_rng(SEED)
    kinds = provinces_kind(ids, provinces)
    old_ids = ids.copy()
    boxes = ndimage.find_objects(ids)
    removed = []
    for pid, row in provinces.items():
        if row['type'] != 'lake':
            continue
        box = tuple(slice(max(s.start - 4, 0), s.stop + 4) for s in boxes[pid - 1])
        own = old_ids[box] == pid
        parts, count = ndimage.label(own & (classes[box] == LAKE), FOUR)
        sizes = ndimage.sum(np.ones(own.shape), parts, np.arange(1, count + 1)) if count else np.zeros(0)
        kept = parts == np.argmax(sizes) + 1 if count else np.zeros_like(own)
        give = own & ~kept
        if not give.any():
            continue
        ring = ndimage.binary_dilation(give, FOUR) & ~give & (kinds[box] == 'land')
        if not ring.any():
            sys.exit(f'{row["name"]}: no land province around it')
        cost = ndimage.distance_transform_edt(give) + BED_NOISE * fractal_noise(give.shape, rng, (16, 6, 2), (1.0, 0.5, 0.25))
        grown = watershed(cost, np.where(ring, old_ids[box], 0), mask=give | ring)
        if (grown[give] == 0).any():            # pockets the land does not reach through `give`: nearest land
            _, (ny, nx) = ndimage.distance_transform_edt(grown == 0, return_indices=True)
            grown = np.where(give & (grown == 0), grown[ny, nx], grown)
        ids[box][give] = grown[give]
        if not kept.any():
            removed.append(pid)
        share = np.unique(grown[give], return_counts=True)
        state = 'removed' if not kept.any() else f'keeps {kept.sum()} blocks'
        print(f'{row["name"]} ({state}): {give.sum()} blocks -> ' +
              ', '.join(f'{provinces[o]["name"]} {n}' for o, n in zip(*share)))
    if (ids == old_ids).all():
        sys.exit('lake provinces already match land.png')

    # Renumber: ids stay 1..N in the old order.
    keep = [pid for pid in sorted(provinces) if pid not in removed]
    remap = np.zeros(ids.max() + 1, np.int32)
    remap[keep] = np.arange(1, len(keep) + 1)
    renumbered = {}
    for new, pid in enumerate(keep, 1):
        renumbered[new] = dict(provinces[pid], id=str(new))
    write(remap[ids], remap[old_ids], renumbered)
    print(f'{len(removed)} lake provinces removed, {len(keep)} provinces left')


def provinces_kind(ids, provinces):
    kinds = np.array([''] + [provinces[pid]['type'] for pid in range(1, ids.max() + 1)], object)
    return kinds[ids]


def main():
    ids, provinces = province_blocks()
    classes, _, _ = read_blocks()
    command = sys.argv[1:2]
    if command == ['split-islands']:
        split_islands(ids, provinces, classes)
    elif command == ['islands-apart']:
        islands_apart(ids, provinces, classes)
    elif command == ['sync-lakes']:
        sync_lakes(ids, provinces, classes)
    elif command == ['give-island'] and len(sys.argv) == 5:
        give_island(ids, provinces, classes, int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
