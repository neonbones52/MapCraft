"""Reading the map sources every tool shares: CSV files (with # comment lines) and the province id per pixel."""
import csv
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def read_csv(path):
    with open(path, encoding='utf-8-sig') as file:
        return list(csv.DictReader(line for line in file if not line.startswith('#')))


def province_map():
    """-> (ids, rows): province id per pixel of provinces.png (0 = a colour not in provinces.csv) and the
    provinces.csv rows by id."""
    rgb = np.asarray(Image.open(ROOT / 'provinces.png').convert('RGB')).astype(np.uint32)
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    rows = read_csv(ROOT / 'provinces.csv')
    lookup = {int(row['color'].lstrip('#'), 16): int(row['id']) for row in rows}
    colors = np.array(sorted(lookup))
    index = np.searchsorted(colors, key.ravel()).clip(0, len(colors) - 1)
    ids = np.where(colors[index] == key.ravel(), np.array([lookup[c] for c in colors])[index], 0).reshape(key.shape)
    return ids.astype(np.int32), {int(row['id']): row for row in rows}
