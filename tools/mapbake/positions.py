"""Locators per province, like EU4 map/positions.txt: city, unit, text and port -> positions.csv.

Runs after tools/labels/provinces.py: the city keeps clear of the province's name where the province has room.
  city   a site of 2 x 2 land blocks of the province (no river or lake) for its one house: the flattest such site
         (ground levels equal), among those the one with the most room round it, CITY_MARGIN px from the province's
         border, the coast, lakes, rivers and the names; city_x, city_y is its centre (map px), city_bx, city_by its
         north-west block, city_facing the way the house faces (0 north, 1 east, 2 south, 3 west), varied by province
  unit   well inside the province, near the city (just off its house, UNIT_DISTANCE), on dry land: a land block (no
         river, lake or sea) whose eight neighbours are land too, off the rivers' water (RIVER_PAD), so the army's
         figure never stands in or over water; level ground first (the eight neighbours on its own ground level);
         where the province has no such point near the city, the next of UNIT_TIERS
  text   the deepest point (water provinces: their only locator besides the unit)
  port   every land province touching a sea province by a block side has a pier: one of its land blocks on the coast
         (pier_bx, pier_by) and the PIER_BLOCKS sea blocks in a row beside it the pier reaches over (pier_dir, as
         the facing), all of one sea province; preferably in open water (sea left, right and ahead), away from the
         neighbouring provinces, then the lowest coast nearest the city (pier_choices). Piers never share or touch sea
         blocks (not even diagonally); provinces with the fewest choices pick first. port_x, port_y is the centre of
         the pier's last block (map px, where ships lie), port_sea its sea province, the province's only way to sea;
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
from heightmap import BLOCK  # noqa: E402
from sources import province_blocks, province_map, read_csv  # noqa: E402
from surface import surface  # noqa: E402
from validate_land import LAND, SEA  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools' / 'mapbake'))
import rivers as river_lines  # noqa: E402

UNIT_DISTANCE = (10, 48)   # px from the city where the army figure stands: just off the house (2 x 2 blocks), not far
CITY_MARGIN = 10           # px the city keeps from its province's border, the coast, lakes and rivers
LABEL_PAD = 2.0            # px the city keeps from the province names' glyph quads, where the province allows
RIVER_PAD = 2.0            # px the city keeps from a river's water
STEPS = ((0, -1), (1, 0), (0, 1), (-1, 0))   # (dx, dy) of facing / pier direction 0 north, 1 east, 2 south, 3 west
PIER_BLOCKS = 2            # sea blocks a pier reaches over
# Where an army point may stand, best first (the first a province has, more than 2 px inside it): a level land block
# with land all round (its eight neighbours on its ground level) near the city (UNIT_DISTANCE); a land block with land
# all round near the city; a level one anywhere in the province; one with land all round anywhere; any dry pixel near
# the city; anywhere; the city's site.
UNIT_TIERS = ('level, near the city', 'firm, near the city', 'level, anywhere', 'firm, anywhere',
              'dry, near the city', 'dry, anywhere', 'the city')


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


def facing(pid):
    """The way a province's house faces: one of the four, scattered over the provinces."""
    return (pid * 2654435761 >> 13) % 4


def city_site(pid, bids, bcls, ground, room):
    """-> (bx, by) of the north-west block of the province's 2 x 2 house site, or None. `room` is the room round
    every map px (higher is better)."""
    ys, xs = np.nonzero(bids == pid)
    if not len(xs):
        return None
    y0, y1, x0, x1 = ys.min(), ys.max() + 2, xs.min(), xs.max() + 2
    own = (bids[y0:y1, x0:x1] == pid) & (bcls[y0:y1, x0:x1] == LAND)
    g = ground[y0:y1, x0:x1].astype(np.int32)
    fits = own[:-1, :-1] & own[1:, :-1] & own[:-1, 1:] & own[1:, 1:]
    corners = np.stack([g[:-1, :-1], g[1:, :-1], g[:-1, 1:], g[1:, 1:]])
    spread = corners.max(axis=0) - corners.min(axis=0)
    if not fits.any():
        fits = own[:-1, :-1]                               # no full site: the house overhangs its neighbours
        if not fits.any():
            return None
    cy, cx = np.nonzero(fits)
    px = np.clip((cx + x0 + 1) * BLOCK, 0, room.shape[1] - 1)
    py = np.clip((cy + y0 + 1) * BLOCK, 0, room.shape[0] - 1)
    score = spread[cy, cx] * 1000.0 - room[py, px]
    k = int(np.argmin(score))
    return int(cx[k] + x0), int(cy[k] + y0)


def pier_choices(pid, bids, bcls, ground, is_sea, site, centre):
    """-> [(score, bx, by, dir)] of the province's coast blocks with PIER_BLOCKS sea blocks of one sea province in
    a row beside them, best first:
      1. open water: the fewest blocks that are not sea beside the pier's blocks (left and right) and before its end;
      2. away from the neighbours: no land of another province round the coast block or beside the pier;
      3. the lowest ground; 4. the nearest to the city."""
    h, w = bids.shape
    out = []

    def inside(x, y):
        return 0 <= x < w and 0 <= y < h

    ys, xs = np.nonzero((bids == pid) & (bcls == LAND))
    for y, x in zip(ys, xs):
        if site and site[0] <= x <= site[0] + 1 and site[1] <= y <= site[1] + 1:
            continue
        for d, (dx, dy) in enumerate(STEPS):
            row = [(x + dx * k, y + dy * k) for k in range(1, PIER_BLOCKS + 1)]
            if not all(inside(nx, ny) and bcls[ny, nx] == SEA and is_sea[bids[ny, nx]] and
                       bids[ny, nx] == bids[row[0][1], row[0][0]] for nx, ny in row):
                continue
            around = [(nx - dy * side, ny + dx * side) for nx, ny in row for side in (-1, 1)]
            around.append((row[-1][0] + dx, row[-1][1] + dy))
            cramped = sum(1 for ax, ay in around if inside(ax, ay) and bcls[ay, ax] != SEA)
            near = around + [(x + ex, y + ey) for ex in (-1, 0, 1) for ey in (-1, 0, 1)]
            foreign = any(inside(ax, ay) and bcls[ay, ax] != SEA and bids[ay, ax] not in (pid, 0) and
                          not is_sea[bids[ay, ax]] for ax, ay in near)
            distance = math.hypot((x + 0.5) * BLOCK - centre[0], (y + 0.5) * BLOCK - centre[1])
            score = cramped * 1e8 + foreign * 1e7 + int(ground[y, x]) * 1e4 + distance
            out.append((score, int(x), int(y), d))
    out.sort()
    return out


def main():
    ids, provinces = province_map()
    bids, _ = province_blocks()
    blocks = surface()
    bcls, ground = blocks['cls'], blocks['ground']
    dry = np.repeat(np.repeat(bcls == LAND, BLOCK, 0), BLOCK, 1)
    # Land blocks with land all round (8 neighbours; past the map's edge counts as land).
    firm_blocks = ndimage.binary_erosion(np.pad(bcls == LAND, 1, constant_values=True), np.ones((3, 3), bool))
    firm = np.repeat(np.repeat(firm_blocks[1:-1, 1:-1], BLOCK, 0), BLOCK, 1)
    # Firm blocks on level ground: the block and its eight neighbours on one ground level.
    padded_ground = np.pad(ground.astype(np.int16), 1, mode='edge')
    level_blocks = firm_blocks[1:-1, 1:-1] & (ndimage.maximum_filter(padded_ground, 3)[1:-1, 1:-1] ==
                                              ndimage.minimum_filter(padded_ground, 3)[1:-1, 1:-1])
    level = np.repeat(np.repeat(level_blocks, BLOCK, 0), BLOCK, 1)
    unit_fallbacks = {}
    pairs = [(int(r['a']), int(r['b'])) for r in read_csv(ROOT / 'adjacency.csv')]
    coastal = {a if provinces[b]['type'] == 'sea' else b for a, b in pairs
               if {provinces[a]['type'], provinces[b]['type']} == {'land', 'sea'}}
    is_sea = np.zeros(max(provinces) + 1, bool)
    is_sea[[pid for pid, row in provinces.items() if row['type'] == 'sea']] = True
    names_file = ROOT / 'build' / 'labels' / 'province_names.npz'
    if not names_file.exists():
        sys.exit(f'{names_file} missing: run tools/labels/provinces.py before positions.py')
    names = lettering(np.load(names_file)['glyphs'], ids.shape)
    wet = river_lines.corridor(ids.shape, river_lines.traced(), RIVER_PAD) if (ROOT / 'rivers.png').exists() \
        else np.zeros(ids.shape, bool)
    boxes = ndimage.find_objects(ids)
    rows = {}
    room_all = np.zeros(ids.shape, np.float32)
    for pid in sorted(provinces):
        row = provinces[pid]
        box = boxes[pid - 1]
        y0, x0 = max(box[0].start - 2, 0), max(box[1].start - 2, 0)
        y1, x1 = min(box[0].stop + 2, ids.shape[0]), min(box[1].stop + 2, ids.shape[1])
        local = ids[y0:y1, x0:x1]
        mask = main_piece(local == pid)
        tx, ty, depth = deepest(mask)
        out = dict(id=pid, type=row['type'], text_x=tx + x0, text_y=ty + y0)
        rows[pid] = out
        if row['type'] != 'land':
            out.update(unit_x=tx + x0, unit_y=ty + y0)
            continue

        # Room for the city: open ground, as far from the names as the province allows.
        open_ground = mask & dry[y0:y1, x0:x1] & ~wet[y0:y1, x0:x1]
        room = ndimage.distance_transform_edt(np.pad(open_ground & ~names[y0:y1, x0:x1], 1))[1:-1, 1:-1]
        if room.max() < CITY_MARGIN:
            room = ndimage.distance_transform_edt(np.pad(open_ground, 1))[1:-1, 1:-1]
        if room.max() <= 0:
            room = depth
        room_all[y0:y1, x0:x1] = np.where(local == pid, room, room_all[y0:y1, x0:x1])
        site = city_site(pid, bids, bcls, ground, room_all)
        if site is None:
            cy, cx = (int(v) for v in np.unravel_index(np.argmax(room), room.shape))
            cx, cy = cx + x0, cy + y0
            print(f'no house site: {row["name"]}')
        else:
            cx, cy = (site[0] + 1) * BLOCK, (site[1] + 1) * BLOCK
            out.update(city_bx=site[0], city_by=site[1], city_facing=facing(pid))
        out.update(city_x=cx, city_y=cy)

        # Unit: well inside the province, near the city, on level dry land first (UNIT_TIERS).
        yy, xx = np.indices(mask.shape)
        distance = np.hypot(xx + x0 - cx, yy + y0 - cy)
        walk = (distance >= UNIT_DISTANCE[0]) & (distance <= UNIT_DISTANCE[1])
        firm_ground = open_ground & firm[y0:y1, x0:x1]
        level_ground = open_ground & level[y0:y1, x0:x1]
        ux, uy = cx - x0, cy - y0   # the last resort: the city's own dry site
        for tier, allowed in enumerate((level_ground & walk, firm_ground & walk, level_ground, firm_ground,
                                        open_ground & walk, open_ground)):
            score = np.where(allowed, depth, -1)
            if score.max() > 2:
                uy, ux = np.unravel_index(np.argmax(score), score.shape)
                break
        else:
            tier = len(UNIT_TIERS) - 1
        if tier > 0:
            unit_fallbacks.setdefault(UNIT_TIERS[tier], []).append(pid)
        out.update(unit_x=int(ux) + x0, unit_y=int(uy) + y0, _site=site)

    # Piers: the provinces with the fewest choices first; a pier's sea block keeps a block clear of every other.
    choices = {pid: pier_choices(pid, bids, bcls, ground, is_sea, rows[pid]['_site'],
                                 (rows[pid]['city_x'], rows[pid]['city_y']))
               for pid in sorted(coastal)}
    taken = np.zeros(bids.shape, bool)
    for pid in sorted(choices, key=lambda p: (len(choices[p]), p)):
        free = [c for c in choices[pid]
                if not any(taken[c[2] + STEPS[c[3]][1] * k, c[1] + STEPS[c[3]][0] * k] for k in range(1, PIER_BLOCKS + 1))]
        if not free:
            print(f'no pier: {provinces[pid]["name"]} - ' +
                  ('no coast block beside its sea' if not choices[pid] else 'every coast block is next to a pier'))
            continue
        _, bx, by, d = free[0]
        for k in range(1, PIER_BLOCKS + 1):
            sx, sy = bx + STEPS[d][0] * k, by + STEPS[d][1] * k
            taken[max(sy - 1, 0):sy + 2, max(sx - 1, 0):sx + 2] = True
        rows[pid].update(pier_bx=bx, pier_by=by, pier_dir=d, port_x=(sx + 0.5) * BLOCK, port_y=(sy + 0.5) * BLOCK,
                         port_sea=int(bids[sy, sx]), port_rotation=round(math.atan2(STEPS[d][1], STEPS[d][0]), 4))

    fields = ['id', 'type', 'city_x', 'city_y', 'city_bx', 'city_by', 'city_facing', 'unit_x', 'unit_y', 'text_x',
              'text_y', 'port_x', 'port_y', 'port_sea', 'port_rotation', 'pier_bx', 'pier_by', 'pier_dir']
    with open(ROOT / 'positions.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields, extrasaction='ignore', lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows[pid] for pid in sorted(rows))
    sites = sum(1 for r in rows.values() if 'city_bx' in r)
    piers = sum(1 for r in rows.values() if 'pier_bx' in r)
    print(f'{len(rows)} provinces, {sites} house sites, {piers} piers -> positions.csv')
    for tier, pids in unit_fallbacks.items():
        print(f'army point {tier}: {len(pids)} provinces ({", ".join(map(str, pids[:12]))}'
              f'{", ..." if len(pids) > 12 else ""})')


if __name__ == '__main__':
    main()
