"""The voxel surface per block, as bake.py packs it (BCLS, BHGT, BWTR, BBIO; docs/MAPFORMAT.md).

Levels count blocks up from the sea floor: SEA_WATER is the top of the sea, a land block of height step s has its
ground top at s + LAND_BASE (one block above the sea). A river's water stands one block below the lowest bank upstream
(the running minimum of the blocks it carves and the dry blocks beside them, so it never stands above a bank), its bed
one block lower; a brook is one block wide, a river two (the second block
beside the path, on its lower side), and a diagonal step of a path fills the lower corner block. A lake's water stands
one block below its lowest shore. Under the sea nothing is drawn: its ground level is 0.

    python tools/mapgen/surface.py      # summary of the surface -> build/surface/levels.png
"""
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

from biomes import read_biomes
from heightmap import ROOT, STEPS, read_blocks
from river_paths import FOUR_STEPS, inside, read_lines, trace
from sources import block_classes
from validate_land import FOUR, LAKE, LAND, SEA, WASTE

RIVER = 4                   # block class of river water (0 sea, 1 land, 2 lake, 3 wasteland as in validate_land)
SEA_WATER = 1               # level of the sea's surface
LAND_BASE = 2               # ground top of a land block of step 0


def lake_levels(classes, steps, ground, water):
    """Every lake (decorative ones too) stands one block below its lowest shore block."""
    lakes, count = ndimage.label(classes == LAKE, FOUR)
    dry = (classes == LAND) | (classes == WASTE)
    for lake, box in enumerate(ndimage.find_objects(lakes), 1):
        grown = tuple(slice(max(s.start - 1, 0), s.stop + 1) for s in box)
        mine = lakes[grown] == lake
        shore = ndimage.binary_dilation(mine, FOUR) & ~mine & dry[grown]
        level = int(steps[grown][shore].min()) + LAND_BASE - 1 if shore.any() else SEA_WATER
        water[grown][mine] = level
        ground[grown][mine] = level - 1


def carve_rivers(classes, steps, paths, cls, ground, water):
    """River blocks: path blocks, the lower corner of every diagonal step, the second block of a 2-wide river. The
    water stands one block below the lowest bank upstream: the running minimum, along the path, of the steps of the
    blocks carved at each point and of the dry blocks beside them (4 neighbours, not carved by any river), so it
    never stands above a bank."""
    dry = (classes == LAND) | (classes == WASTE)

    # Which blocks each point of each river carves (the side block of a 2-wide river is the lower one; a diagonal
    # step fills the lower corner).
    carved_by = []
    for river in paths:
        path, widths = [tuple(map(int, p)) for p in river['path']], river['width']
        points = []
        for i, (block, width) in enumerate(zip(path, widths)):
            blocks, corner = [block], None
            if i + 1 < len(path):
                (y0, x0), (y1, x1) = block, path[i + 1]
                if y0 != y1 and x0 != x1:
                    a, b = (y0, x1), (y1, x0)
                    corner = a if steps[a] <= steps[b] else b
            if width == 2:
                before, after = path[max(i - 1, 0)], path[min(i + 1, len(path) - 1)]
                dy, dx = after[0] - before[0], after[1] - before[1]
                y, x = block
                across = ((0, -1), (0, 1)) if abs(dy) >= abs(dx) else ((-1, 0), (1, 0))
                sides = [(y + oy, x + ox) for oy, ox in across]
                sides = [s for s in sides if inside(*s) and dry[s] and s not in path]
                if sides:
                    blocks.append(min(sides, key=lambda s: steps[s]))
            points.append((blocks, corner))
        carved_by.append(points)
    carved = np.zeros(classes.shape, bool)
    for points in carved_by:
        for blocks, corner in points:
            for block in blocks + ([corner] if corner else []):
                if dry[block]:
                    carved[block] = True

    def lowest_bank(blocks):
        low = min(int(steps[b]) for b in blocks)
        for y, x in blocks:
            for oy, ox in FOUR_STEPS:
                n = (y + oy, x + ox)
                if inside(*n) and dry[n] and not carved[n]:
                    low = min(low, int(steps[n]))
        return low

    def set_block(block, level):
        if not dry[block]:
            return
        if cls[block] == RIVER:
            level = min(level, int(water[block]))
        cls[block] = RIVER
        water[block] = level
        ground[block] = level - 1

    for points in carved_by:
        low, levels = STEPS + 1, []
        for i, (blocks, corner) in enumerate(points):
            low = min(low, lowest_bank(blocks + ([corner] if corner else [])))
            levels.append(low + LAND_BASE - 1)
        for i, (blocks, corner) in enumerate(points):
            for block in blocks:
                set_block(block, levels[i])
            if corner:
                set_block(corner, min(levels[i], levels[i + 1]))


def surface():
    """-> dict(cls, ground, water, biome per block; biome_keys: '' + biomes.csv keys)."""
    classes, steps, _ = read_blocks()
    cls = block_classes()
    biomes, rows = read_biomes()
    ground = np.zeros(classes.shape, np.uint8)
    water = np.zeros(classes.shape, np.uint8)
    dry = (cls == LAND) | (cls == WASTE)
    ground[dry] = steps[dry] + LAND_BASE
    water[cls == SEA] = SEA_WATER
    lake_levels(cls, steps, ground, water)
    kind, problems = read_lines()
    if problems:
        raise ValueError('rivers.png: ' + '; '.join(problems))
    paths, _ = trace(kind, classes)
    carve_rivers(cls, steps, paths, cls, ground, water)
    return dict(cls=cls, ground=ground, water=water, biome=biomes, biome_keys=[''] + [r['key'] for r in rows])


def main():
    try:
        s = surface()
    except (ValueError, FileNotFoundError) as error:
        sys.stderr.write(f'{error}\n')
        return 1
    cls, ground, water = s['cls'], s['ground'], s['water']
    names = {SEA: 'sea', LAND: 'land', LAKE: 'lake', WASTE: 'wasteland', RIVER: 'river'}
    counts = ', '.join(f'{names[k]} {int((cls == k).sum())}' for k in names)
    print(f'blocks: {counts}')
    print(f'ground levels {int(ground[ground > 0].min())}..{int(ground.max())}, '
          f'river water {int(water[cls == RIVER].min())}..{int(water[cls == RIVER].max())}, '
          f'lake water {int(water[cls == LAKE].min())}..{int(water[cls == LAKE].max())}')
    top = np.maximum(ground, water).astype(np.int32)
    rgb = np.repeat((40 + top * 200 // int(top.max()))[..., None], 3, axis=2).astype(np.uint8)
    rgb[cls == SEA] = (20, 40, 90)
    rgb[cls == LAKE] = (60, 140, 230)
    rgb[cls == RIVER] = (40, 120, 255)
    folder = ROOT / 'build' / 'surface'
    folder.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb).resize((cls.shape[1] * 2, cls.shape[0] * 2), Image.NEAREST).save(folder / 'levels.png')
    print('wrote build/surface/levels.png')
    return 0


if __name__ == '__main__':
    sys.exit(main())
