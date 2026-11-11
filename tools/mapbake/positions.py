"""Locators per province, like EU4 map/positions.txt: city, unit, text and port in map pixels -> positions.csv.

Runs after tools/labels/provinces.py: the city keeps clear of the province's name where the province has room.
  city   the deepest point of the province's main piece, CITY_MARGIN px from its border, the coast, lakes and rivers
  unit   well inside the province, a short walk from the city
  text   the deepest point (water provinces: their only locator besides the unit)
  port   every land province touching a sea province has one (lakes don't count): the coast pixel nearest the city,
         its point PORT_OUT px out over the sea along the coast's normal; port_sea is the sea province there and
         port_rotation the direction from the land to the sea (radians, x right / y down)

    python tools/mapbake/positions.py
"""
import csv
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from generate_provinces import read_classes  # noqa: E402
from sources import province_map, read_csv  # noqa: E402
from validate_land import LAND  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools' / 'mapbake'))
import rivers as river_lines  # noqa: E402

UNIT_DISTANCE = (22, 48)   # px from the city where the army figure stands
CITY_MARGIN = 10           # px the city keeps from its province's border, the coast, lakes and rivers
LABEL_PAD = 2.0            # px the city keeps from the province names' glyph quads, where the province allows
RIVER_PAD = 2.0            # px the city keeps from a river's water
PORT_SEARCH = 60           # px of margin round the province's box searched for the coast
PORT_OUT = 3.0             # px from the coast out over the sea to the port point (where ships lie)


def deepest(mask):
    """Pixel of the mask farthest from its edge (pole of inaccessibility on the pixel grid) and that depth."""
    depth = ndimage.distance_transform_edt(np.pad(mask, 1))[1:-1, 1:-1]
    y, x = np.unravel_index(np.argmax(depth), depth.shape)
    return int(x), int(y), depth


def main_piece(mask):
    labels, count = ndimage.label(mask)
    if count <= 1:
        return mask
    return labels == (np.argmax(np.bincount(labels.ravel())[1:]) + 1)


def lettering(glyphs, shape):
    """Pixels of the glyph quads (rows x, y, reading direction x, y, width, height, code; map px) grown by
    LABEL_PAD."""
    image = Image.new('L', (shape[1], shape[0]))
    draw = ImageDraw.Draw(image)
    for x, y, dx, dy, w, h, _ in glyphs:
        ux, uy = dy, -dx                                   # "up" of the text in map px (y down)
        corners = [(x + dx * a * w / 2 + ux * b * h / 2, y + dy * a * w / 2 + uy * b * h / 2)
                   for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        draw.polygon(corners, fill=255)
    return ndimage.distance_transform_edt(~(np.asarray(image) > 0)) <= LABEL_PAD


def port(local, own, sea, cx, cy):
    """-> (x, y, angle) in window px: the coast pixel of `own` nearest (cx, cy) pushed PORT_OUT px out to sea, and
    the direction from the land to the sea; None without a coast."""
    shore = own & ndimage.binary_dilation(sea)
    if not shore.any():
        return None
    # The coast's normal: the gradient of the blurred sea mask points out to open water.
    soft = ndimage.gaussian_filter(sea.astype(np.float32), 3.0)
    gy, gx = np.gradient(soft)
    sy, sx = np.nonzero(shore)
    h, w = local.shape
    for k in np.argsort((sx - cx) ** 2 + (sy - cy) ** 2):
        x, y = int(sx[k]), int(sy[k])
        norm = math.hypot(gx[y, x], gy[y, x])
        if norm < 1e-4:
            continue
        nx, ny = gx[y, x] / norm, gy[y, x] / norm
        px, py = x + 0.5 + nx * PORT_OUT, y + 0.5 + ny * PORT_OUT
        if 0 <= px < w and 0 <= py < h and sea[int(py), int(px)]:
            return px, py, math.atan2(ny, nx)
    return None


def main():
    ids, provinces = province_map()
    dry = read_classes(ROOT / 'land.png') == LAND
    pairs = [(int(r['a']), int(r['b'])) for r in read_csv(ROOT / 'adjacency.csv')]
    coastal = {a if provinces[b]['type'] == 'sea' else b for a, b in pairs
               if {provinces[a]['type'], provinces[b]['type']} == {'land', 'sea'}}
    sea_ids = [pid for pid, row in provinces.items() if row['type'] == 'sea']
    names_file = ROOT / 'build' / 'labels' / 'province_names.npz'
    if not names_file.exists():
        sys.exit(f'{names_file} missing: run tools/labels/provinces.py before positions.py')
    names = lettering(np.load(names_file)['glyphs'], ids.shape)
    wet = river_lines.corridor(ids.shape, river_lines.traced(), RIVER_PAD) if (ROOT / 'rivers.png').exists() \
        else np.zeros(ids.shape, bool)
    boxes = ndimage.find_objects(ids)
    rows = []
    for pid in sorted(provinces):
        row = provinces[pid]
        box = boxes[pid - 1]
        pad = PORT_SEARCH + 2
        y0, x0 = max(box[0].start - pad, 0), max(box[1].start - pad, 0)
        y1, x1 = min(box[0].stop + pad, ids.shape[0]), min(box[1].stop + pad, ids.shape[1])
        local = ids[y0:y1, x0:x1]
        mask = main_piece(local == pid)
        tx, ty, depth = deepest(mask)
        out = dict(id=pid, type=row['type'], text_x=tx + x0, text_y=ty + y0)
        if row['type'] != 'land':
            out.update(unit_x=tx + x0, unit_y=ty + y0)
            rows.append(out)
            continue

        # City: the deepest open ground, as far from the names as the province allows.
        open_ground = mask & dry[y0:y1, x0:x1] & ~wet[y0:y1, x0:x1]
        room = ndimage.distance_transform_edt(np.pad(open_ground & ~names[y0:y1, x0:x1], 1))[1:-1, 1:-1]
        if room.max() < CITY_MARGIN:
            room = ndimage.distance_transform_edt(np.pad(open_ground, 1))[1:-1, 1:-1]
        if room.max() <= 0:
            room = depth
        cy, cx = (int(v) for v in np.unravel_index(np.argmax(room), room.shape))
        out.update(city_x=cx + x0, city_y=cy + y0)

        # Unit: well inside the province, a short walk away from the city.
        yy, xx = np.indices(mask.shape)
        distance = np.hypot(xx - cx, yy - cy)
        score = np.where(mask & (distance >= UNIT_DISTANCE[0]) & (distance <= UNIT_DISTANCE[1]), depth, -1)
        uy, ux = np.unravel_index(np.argmax(score), score.shape) if score.max() > 2 else (cy, cx)
        out.update(unit_x=int(ux) + x0, unit_y=int(uy) + y0)

        if pid in coastal:
            found = port(local, (local == pid) & dry[y0:y1, x0:x1], np.isin(local, sea_ids), cx, cy)
            if found:
                px, py, angle = found
                out.update(port_x=round(px + x0, 1), port_y=round(py + y0, 1),
                           port_sea=int(local[int(py), int(px)]), port_rotation=round(angle, 4))
            else:
                print(f'no port: {row["name"]} - no coast pixel facing its sea neighbour')
        rows.append(out)

    fields = ['id', 'type', 'city_x', 'city_y', 'unit_x', 'unit_y', 'text_x', 'text_y', 'port_x', 'port_y',
              'port_sea', 'port_rotation']
    with open(ROOT / 'positions.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    ports = sum(1 for r in rows if 'port_x' in r)
    print(f'{len(rows)} provinces, {ports} ports -> positions.csv')


if __name__ == '__main__':
    main()
