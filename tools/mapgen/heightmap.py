"""heightmap.png (Azgaar's Fantasy Map Generator export) -> the block grid: land, sea and lakes per block and the height
of every land block in steps. Writes land.png (one pixel per block) for the user to paint the other sources over.

heightmap.png is the PNG export of Azgaar's map with only the heightmap layer on, in the "monochrome" scheme: map size
and viewport 1152x576, scale x4 -> 4608x2304, one export pixel per map pixel, BLOCK x BLOCK pixels per block.
Azgaar draws a height h (land 20..100) as d3.interpolateGreys(1 - h/100); the grey is inverted through that curve.
Greys below the sea level are the sea depth bands and the anti-aliasing along the coast: water.

A block is land when most of its pixels are; its step is round((mean h - 20) / H_PER_STEP), so h 100 is STEPS.
Water that does not reach the map edge is a lake. A 2x2 of blocks touching only diagonally is resolved by flipping
the block whose vote was the closest to a tie.

    python tools/mapgen/heightmap.py      # heightmap.png -> land.png + build/heightmap/steps.png (preview)
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

from validate_land import FOUR, HEIGHT, LAKE, LAND, PALETTE, SEA, WIDTH

ROOT = Path(__file__).resolve().parents[2]
BLOCK = 4                   # export px per block side
SEA_LEVEL = 20              # Azgaar height of the coast
H_PER_STEP = 2.5            # Azgaar height per block step
STEPS = 32                  # step of h = 100
SCHEME_TOLERANCE = 2.0      # grey levels a land pixel may differ from the scheme
MAX_OFF_SCHEME = 0.001      # share of land pixels allowed off the scheme (anti-aliasing between bands)

GREYS = (255, 240, 217, 189, 150, 115, 82, 37, 0)  # d3-scale-chromatic schemeGreys[9]


def greys(t):
    """d3.interpolateGreys(t): interpolateRgbBasis over GREYS, one channel."""
    n = len(GREYS) - 1
    t = min(max(t, 0.0), 1.0)
    i = n - 1 if t >= 1.0 else int(t * n)
    v1, v2 = GREYS[i], GREYS[i + 1]
    v0 = GREYS[i - 1] if i > 0 else 2 * v1 - v2
    v3 = GREYS[i + 2] if i < n - 1 else 2 * v2 - v1
    t1 = (t - i / n) * n
    t2, t3 = t1 * t1, t1 * t1 * t1
    return ((1 - 3 * t1 + 3 * t2 - t3) * v0 + (4 - 6 * t2 + 3 * t3) * v1 + (1 + 3 * t1 + 3 * t2 - 3 * t3) * v2
            + t3 * v3) / 6


def scheme():
    """-> (grey of every land height SEA_LEVEL..100, Azgaar height of every grey 0..255, distance of that grey
    to the scheme)."""
    heights = np.arange(SEA_LEVEL, 101)
    grey_of = np.array([greys(1 - h / 100) for h in heights])
    distance = np.abs(np.arange(256)[:, None] - grey_of[None, :])
    return grey_of, heights[distance.argmin(axis=1)], distance.min(axis=1)


def read_export(path):
    """-> (grey per export pixel, problems). Problems are format errors: the file is unusable while they last."""
    image = Image.open(path)
    rgba = np.asarray(image.convert('RGBA')).astype(np.int16)
    problems = []
    if (image.width, image.height) != (WIDTH * BLOCK, HEIGHT * BLOCK):
        problems.append(f'size {image.width}x{image.height}, must be {WIDTH * BLOCK}x{HEIGHT * BLOCK}: '
                        f'Azgaar map size and viewport {WIDTH}x{HEIGHT}, PNG scale x{BLOCK}')
    if (rgba[..., 3] != 255).any():
        problems.append('transparent pixels')
    if (rgba[..., 0] != rgba[..., 1]).any() or (rgba[..., 1] != rgba[..., 2]).any():
        problems.append('not grey: only the heightmap layer in the monochrome scheme')
    return rgba[..., 0].astype(np.uint8), problems


def checkerboards(land):
    """-> (y, x) of the top-left corner of every 2x2 whose land touches only diagonally."""
    a, b, c, d = land[:-1, :-1], land[:-1, 1:], land[1:, :-1], land[1:, 1:]
    return np.argwhere((a & d & ~b & ~c) | (b & c & ~a & ~d))


def read_blocks(path=ROOT / 'heightmap.png'):
    """-> (classes per block: SEA, LAND or LAKE; step per land block, 0 on water; report lines). Raises ValueError
    when the export is unusable."""
    grey, problems = read_export(path)
    if problems:
        raise ValueError('; '.join(problems))
    grey_of, height_of, distance = scheme()
    land_px = grey >= grey_of[0] - 1.5
    off = (distance[grey] > SCHEME_TOLERANCE) & land_px
    if off.sum() > MAX_OFF_SCHEME * land_px.sum():
        raise ValueError(f'{int(off.sum())} land pixels off the monochrome scheme: the heightmap layer style changed')

    shape = (HEIGHT, BLOCK, WIDTH, BLOCK)
    count = land_px.reshape(shape).sum(axis=(1, 3))
    vote = count / (BLOCK * BLOCK)
    total = np.where(land_px, height_of[grey], 0).astype(np.int64).reshape(shape).sum(axis=(1, 3))
    mean = np.divide(total, count, out=np.full(vote.shape, float(SEA_LEVEL)), where=count > 0)
    land = vote >= 0.5

    flips = 0
    for _ in range(16):
        spots = checkerboards(land)
        if not len(spots):
            break
        for y, x in spots:
            cells = [(y, x), (y, x + 1), (y + 1, x), (y + 1, x + 1)]
            if not len(checkerboards(land[y:y + 2, x:x + 2])):
                continue
            cy, cx = min(cells, key=lambda cell: abs(vote[cell] - 0.5))
            land[cy, cx] = not land[cy, cx]
            flips += 1
    else:
        raise ValueError('diagonal-only contacts did not resolve')

    steps = np.clip(np.rint((mean - SEA_LEVEL) / H_PER_STEP), 0, STEPS).astype(np.uint8)
    steps[~land] = 0

    water, count_water = ndimage.label(~land, FOUR)
    edge = np.unique(np.concatenate([water[0], water[-1], water[:, 0], water[:, -1]]))
    classes = np.full(land.shape, LAND, np.uint8)
    classes[~land] = LAKE
    classes[np.isin(water, edge[edge > 0])] = SEA

    histogram = np.bincount(steps[land], minlength=STEPS + 1)
    lakes = np.bincount(water[classes == LAKE])
    lines = [
        f'{path.name}: {grey.shape[1]}x{grey.shape[0]} -> {WIDTH}x{HEIGHT} blocks',
        f'land {int(land.sum())} blocks ({100 * land.mean():.1f} %), sea {int((classes == SEA).sum())}, '
        f'lakes {int((classes == LAKE).sum())} in {int((lakes > 0).sum())}',
        f'{flips} blocks flipped to break diagonal-only contacts',
        f'land pixels off the scheme: {int(off.sum())}',
        'land blocks per step: ' + ' '.join(f'{s}:{n}' for s, n in enumerate(histogram)),
    ]
    return classes, steps, lines


def preview(classes, steps):
    """Steps as greys, sea dark blue, lakes light blue."""
    rgb = np.empty(classes.shape + (3,), np.uint8)
    level = (48 + steps.astype(np.int32) * 207 // STEPS).astype(np.uint8)
    rgb[...] = level[..., None]
    rgb[classes == SEA] = (20, 40, 90)
    rgb[classes == LAKE] = (60, 140, 230)
    return Image.fromarray(rgb)


def main():
    try:
        classes, steps, lines = read_blocks()
    except (ValueError, FileNotFoundError) as error:
        sys.stderr.write(f'heightmap.png: {error}\n')
        return 1
    colors = {value: color for color, value in PALETTE.items()}
    rgb = np.zeros(classes.shape + (3,), np.uint8)
    for value in (SEA, LAND, LAKE):
        rgb[classes == value] = colors[value]
    Image.fromarray(rgb).save(ROOT / 'land.png')
    out = ROOT / 'build' / 'heightmap'
    out.mkdir(parents=True, exist_ok=True)
    preview(classes, steps).save(out / 'steps.png')
    sys.stdout.write('\n'.join(lines) + '\nwrote land.png, build/heightmap/steps.png\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
