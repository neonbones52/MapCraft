"""borders.png: the lines the user draws over the land (one pixel per block), and what they cut the land into.

borders.png, transparent except the lines; a pixel is a line when its colour is exact and it is at least half opaque:
    (255, 0, 0)    border of a land (a group of regions, `parent` in regions.csv)
    (255, 255, 0)  border of a region, optional: regions without one are cut by the province generator
    (0, 255, 255)  border of a province: a piece of a region cut off by it is exactly one province
Lines of any width, diagonal steps allowed (an 8-connected line still cuts 4-connected land). A line cuts where it
reaches the coast, the wasteland, the map edge or another line of the same or a higher rank; an end the land flows
around is reported.
The passable land is land.png's land without wasteland.png's wasteland; lines elsewhere are ignored.

Lands are the pieces of passable land between land lines; the blocks of a stroke go to the nearest piece, so a border
runs down the middle of the stroke. A piece smaller than MIN_LAND_SHARE of a land's expected size joins, whole, the
nearest land; an island belongs whole to one land. Within a region, the largest piece left by province lines is the
remainder the generator cuts into provinces; every other piece is one fixed province.

    python tools/mapgen/borders.py      # check borders.png against land.png and wasteland.png -> build/borders/
"""
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from skimage.morphology import skeletonize

from generate_provinces import distinct_colors
from heightmap import ROOT, read_blocks
from validate_land import FOUR, LAND, PALETTE, Report, WASTE, load

LAND_LINE, REGION_LINE, PROVINCE_LINE = 1, 2, 3
COLORS = {(255, 0, 0): LAND_LINE, (255, 255, 0): REGION_LINE, (0, 255, 255): PROVINCE_LINE}
NAMES = {LAND_LINE: 'land', REGION_LINE: 'region', PROVINCE_LINE: 'province'}
LAND_PROVINCES = 850            # provinces on the passable land
LANDS = 18                      # expected lands, for the size of a sliver
MIN_LAND_SHARE = 0.1            # pieces below this share of an expected land join the nearest land
FIXED_PROVINCE = (0.15, 3.0)    # a fixed province outside these multiples of an average province is reported
END_WINDOW = 5                  # blocks around a line end where the land must not flow around it


def passable_land():
    """-> (land mask per block, wasteland included; passable mask: land.png land without wasteland.png wasteland).
    Raises ValueError when wasteland.png is broken or differs from land.png outside the wasteland."""
    classes, _, _ = read_blocks()
    report = Report()
    waste = load(ROOT / 'wasteland.png', report)
    if report.errors:
        raise ValueError('wasteland.png: ' + '; '.join(line.strip() for line in report.lines if line.startswith(' E')))
    same = np.where(waste == WASTE, LAND, waste) == classes
    if not same.all():
        y, x = np.argwhere(~same)[0]
        raise ValueError(f'wasteland.png differs from land.png outside the wasteland at {int((~same).sum())} blocks, '
                         f'first at ({x}, {y})')
    return classes == LAND, (classes == LAND) & (waste != WASTE)


def read_borders(path=ROOT / 'borders.png'):
    """-> (line kind per block: 0 or LAND_LINE / REGION_LINE / PROVINCE_LINE, problems)."""
    rgba = np.asarray(Image.open(path).convert('RGBA')).astype(np.uint32)
    problems = []
    if rgba.shape[:2] != (576, 1152):
        problems.append(f'size {rgba.shape[1]}x{rgba.shape[0]}, must be 1152x576')
    key = (rgba[..., 0] << 16) | (rgba[..., 1] << 8) | rgba[..., 2]
    drawn = rgba[..., 3] >= 128
    lines = np.zeros(key.shape, np.uint8)
    for (r, g, b), kind in COLORS.items():
        lines[drawn & (key == ((r << 16) | (g << 8) | b))] = kind
    stray = drawn & (lines == 0)
    if stray.any():
        y, x = np.argwhere(stray)[0]
        problems.append(f'{int(stray.sum())} drawn pixels of other colours, first at ({x}, {y})')
    return lines, problems


def pieces(passable, cut):
    """-> (label per block of the passable land left by `cut`, area per label with [0] unused)."""
    labels, _ = ndimage.label(passable & ~cut, FOUR)
    return labels, np.bincount(labels.ravel())


def loose_ends(lines, passable, kinds):
    """-> [(x, y, kind)] ends of the lines of `kinds` the land flows around: within END_WINDOW of the end of the line's
    skeleton, the passable land beside the line is one 4-connected piece. An end that reaches the coast, the
    wasteland, the map edge or a line of the same or a higher rank splits that land and is not reported."""
    found = []
    r = END_WINDOW
    for kind in kinds:
        barrier = (lines > 0) & (lines <= kind)
        skeleton = skeletonize(barrier)
        neighbours = ndimage.convolve(skeleton.astype(np.uint8), np.ones((3, 3), np.uint8), mode='constant') - 1
        for y, x in np.argwhere(skeleton & (neighbours <= 1)):
            if lines[y, x] != kind:
                continue
            box = (slice(max(y - r, 0), y + r + 1), slice(max(x - r, 0), x + r + 1))
            wall = barrier[box]
            own, _ = ndimage.label(wall, np.ones((3, 3), bool))
            line = own == own[y - box[0].start, x - box[1].start]
            beside = ndimage.binary_dilation(line, FOUR) & passable[box] & ~wall
            sides, _ = ndimage.label(passable[box] & ~wall, FOUR)
            if len(np.unique(sides[beside])) == 1:
                found.append((int(x), int(y), kind))
    return found


def nearest_piece(labels, passable):
    """Every passable block without a label (a line block) takes the label of the nearest labelled block, so a thick
    stroke is split down its middle."""
    _, (iy, ix) = ndimage.distance_transform_edt(labels == 0, return_indices=True)
    return np.where(passable, labels[iy, ix], 0)


def assign_lands(land, passable, lines):
    """-> (land label per passable block, labels of the lands, count of slivers joined). Land lines split the
    passable land into pieces; pieces of at least MIN_LAND_SHARE of an expected land are lands. A smaller piece joins,
    whole, the land nearest to it. An island (a landmass, wasteland included, with less passable land than an
    expected land) belongs whole to one land: the one holding most of it."""
    labels, areas = pieces(passable, lines == LAND_LINE)
    labels = nearest_piece(labels, passable)
    areas = np.bincount(labels.ravel(), minlength=len(areas))
    expected = passable.sum() / LANDS
    major = [i for i in range(1, len(areas)) if areas[i] >= MIN_LAND_SHARE * expected]
    keep = np.isin(labels, major)
    distance, (iy, ix) = ndimage.distance_transform_edt(~keep, return_indices=True)
    slivers = 0
    for piece, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None or piece in major:
            continue
        ys, xs = np.nonzero(labels[box] == piece)
        k = np.argmin(distance[box][ys, xs])
        near = labels[iy[box][ys[k], xs[k]], ix[box][ys[k], xs[k]]]
        labels[box][labels[box] == piece] = near
        slivers += 1
    masses, _ = ndimage.label(land, FOUR)
    for mass, box in enumerate(ndimage.find_objects(masses), 1):
        mine = (masses[box] == mass) & passable[box]
        inside = labels[box][mine]
        if 0 < inside.size < expected:
            labels[box][mine] = np.bincount(inside).argmax()
    return labels, [m for m in major if (labels == m).any()], slivers


def cells_in_regions(passable, lines, regions):
    """-> cell label per passable block: the pieces left by all lines, line blocks given to the nearest piece of
    their own region, so every cell lies inside one region."""
    raw, _ = pieces(passable, lines > 0)
    cells = np.zeros_like(raw)
    for region, box in enumerate(ndimage.find_objects(regions), 1):
        if box is None:
            continue
        mine = regions[box] == region
        local = np.where(mine, raw[box], 0)
        _, (iy, ix) = ndimage.distance_transform_edt(local == 0, return_indices=True)
        cells[box][mine] = local[iy, ix][mine]
    return cells


def analyse(land, passable, lines):
    """-> (land label per passable block, cell label per passable block (pieces left by all lines), lands
    [(label, area)], joined slivers, fixed provinces [(x, y, area)], average province area)."""
    target = passable.sum() / LAND_PROVINCES
    lands, major, slivers = assign_lands(land, passable, lines)

    regions = nearest_piece(pieces(passable, (lines == LAND_LINE) | (lines == REGION_LINE))[0], passable)
    cells = cells_in_regions(passable, lines, regions)
    cell_areas = np.bincount(cells.ravel())
    fixed = []
    for region in range(1, regions.max() + 1):
        inside = np.unique(cells[regions == region])
        inside = inside[inside > 0]
        if len(inside) < 2:
            continue
        for cell in sorted(inside, key=lambda c: -cell_areas[c])[1:]:
            y, x = ndimage.center_of_mass(cells == cell)
            fixed.append((int(x), int(y), int(cell_areas[cell])))
    return lands, cells, [(m, int((lands == m).sum())) for m in major], slivers, fixed, target


def edges(labels, scale):
    """Pixels of the image `scale` times larger where the label changes to the right or below: 1 px borders."""
    big = labels.repeat(scale, axis=0).repeat(scale, axis=1)
    edge = np.zeros(big.shape, bool)
    edge[:, :-1] |= big[:, :-1] != big[:, 1:]
    edge[:-1, :] |= big[:-1, :] != big[1:, :]
    return edge & (big > 0)


def preview(land, passable, lands, cells, ends, fixed, scale=2):
    """Lands in colours with their borders (black) and the province lines' borders (white) as they will be cut:
    one block edge, down the middle of the strokes."""
    rgb = np.zeros(lands.shape + (3,), np.uint8)
    rgb[...] = (20, 40, 90)
    colors = distinct_colors(int(lands.max()) + 1, seed=5)
    for label in range(1, int(lands.max()) + 1):
        rgb[lands == label] = colors[label]
    rgb[land & ~passable] = (110, 110, 110)
    big = rgb.repeat(scale, axis=0).repeat(scale, axis=1)
    big[edges(cells, scale)] = (255, 255, 255)
    big[edges(lands, scale)] = (0, 0, 0)
    image = Image.fromarray(big)
    draw = ImageDraw.Draw(image)
    for x, y, _ in ends:
        draw.ellipse((2 * x - 10, 2 * y - 10, 2 * x + 10, 2 * y + 10), outline=(255, 120, 0), width=3)
    for x, y, _ in fixed:
        draw.rectangle((2 * x - 3, 2 * y - 3, 2 * x + 3, 2 * y + 3), fill=(255, 255, 255), outline=(0, 0, 0))
    return image


def main():
    try:
        land, passable = passable_land()
    except (ValueError, FileNotFoundError) as error:
        sys.stderr.write(f'{error}\n')
        return 1
    lines, problems = read_borders()
    if problems:
        sys.stderr.write('borders.png: ' + '; '.join(problems) + '\n')
        return 1
    out = []
    for kind, name in NAMES.items():
        drawn = lines == kind
        out.append(f'{name} lines: {int(drawn.sum())} blocks, {int((drawn & ~passable).sum())} of them off the '
                   f'passable land (ignored)')
    lands, cells, majors, slivers, fixed, target = analyse(land, passable, lines)
    out.append(f'passable land {int(passable.sum())} blocks; average province {target:.0f} blocks '
               f'({LAND_PROVINCES} provinces)')
    out.append(f'{len(majors)} lands, {slivers} slivers and islands joined the nearest land:')
    for label, area in sorted(majors, key=lambda item: -item[1]):
        y, x = ndimage.center_of_mass(lands == label)
        out.append(f'  land at ({x:4.0f}, {y:4.0f}): {area:6} blocks, ~{area / target:.0f} provinces')
    ends = loose_ends(lines, passable, (LAND_LINE, REGION_LINE, PROVINCE_LINE))
    out.append(f'{len(ends)} loose line ends' + (':' if ends else ''))
    for x, y, kind in ends:
        out.append(f'  {NAMES[kind]} line ends at ({x}, {y})')
    out.append(f'{len(fixed)} fixed provinces (cut off by province lines):')
    for x, y, area in sorted(fixed):
        low, high = FIXED_PROVINCE
        note = '  small' if area < low * target else '  large' if area > high * target else ''
        out.append(f'  at ({x:4}, {y:4}): {area:5} blocks, {area / target:.1f} x average{note}')
    folder = ROOT / 'build' / 'borders'
    folder.mkdir(parents=True, exist_ok=True)
    preview(land, passable, lands, cells, ends, fixed).save(folder / 'lands.png')
    out.append('wrote build/borders/lands.png')
    sys.stdout.write('\n'.join(out) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
