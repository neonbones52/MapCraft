"""biome.png: the biome of every land block (one pixel per block), in the colours of data/common/biomes.csv.

Every land block of land.png, wasteland included, must carry one of the table's colours; water is not read.

    python tools/mapgen/biomes.py      # check biome.png -> build/biomes/biomes.png
"""
import sys

import numpy as np
from PIL import Image

from heightmap import ROOT, read_blocks
from sources import read_csv
from validate_land import HEIGHT, LAKE, LAND, SEA, WIDTH

TABLE = ROOT / 'data' / 'common' / 'biomes.csv'


def read_table():
    """-> biomes.csv rows; biome index i + 1 is row i (0 = no biome)."""
    return read_csv(TABLE)


def read_biomes(path=ROOT / 'biome.png'):
    """-> (biome index per block, 0 on water; table rows). Raises ValueError when biome.png is unusable."""
    rows = read_table()
    classes, _, _ = read_blocks()
    rgb = np.asarray(Image.open(path).convert('RGB')).astype(np.uint32)
    if rgb.shape[:2] != (HEIGHT, WIDTH):
        raise ValueError(f'size {rgb.shape[1]}x{rgb.shape[0]}, must be {WIDTH}x{HEIGHT}')
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    biomes = np.zeros(key.shape, np.uint8)
    for index, row in enumerate(rows, 1):
        biomes[key == int(row['color'].lstrip('#'), 16)] = index
    land = classes == LAND
    biomes[~land] = 0
    missing = land & (biomes == 0)
    if missing.any():
        y, x = np.argwhere(missing)[0]
        raise ValueError(f'{int(missing.sum())} land blocks without a biome colour, first at ({x}, {y})')
    return biomes, rows


def preview(biomes, rows, classes):
    colors = np.array([(0, 0, 0)] + [tuple(int(r['color'][i:i + 2], 16) for i in (1, 3, 5)) for r in rows], np.uint8)
    rgb = colors[biomes]
    rgb[classes == SEA] = (20, 40, 90)
    rgb[classes == LAKE] = (60, 140, 230)
    return Image.fromarray(rgb).resize((WIDTH * 2, HEIGHT * 2), Image.NEAREST)


def main():
    try:
        biomes, rows = read_biomes()
    except (ValueError, FileNotFoundError) as error:
        sys.stderr.write(f'biome.png: {error}\n')
        return 1
    counts = np.bincount(biomes.ravel(), minlength=len(rows) + 1)
    land = counts[1:].sum()
    lines = [f'{land} land blocks, all with a biome']
    for index, row in enumerate(rows, 1):
        lines.append(f'  {row["name"]:10} {counts[index]:7} blocks  {100 * counts[index] / land:5.1f} %')
    folder = ROOT / 'build' / 'biomes'
    folder.mkdir(parents=True, exist_ok=True)
    classes, _, _ = read_blocks()
    preview(biomes, rows, classes).save(folder / 'biomes.png')
    lines.append('wrote build/biomes/biomes.png')
    sys.stdout.write('\n'.join(lines) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
