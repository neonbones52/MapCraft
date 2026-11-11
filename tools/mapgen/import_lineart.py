"""Convert anti-aliased line art (land fill, white water, dark borders) into land.png + regions.png."""
import argparse
import csv
import colorsys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

from validate_land import FOUR, HEIGHT, LAKE, LAND, PALETTE, SEA, WIDTH

LAND_FILL = (0xD1, 0xDB, 0xDD)
WATER_FILL = (0xFF, 0xFF, 0xFF)
TOLERANCE = 12
MIN_FILL = 30
MIN_REGION = 2500


def classify(rgb):
    """0 = line or anti-aliasing, 1 = land fill, 2 = water fill."""
    rgb = rgb.astype(np.int16)
    near = lambda fill: (np.abs(rgb - np.array(fill, np.int16)).max(axis=2) <= TOLERANCE)
    kind = np.zeros(rgb.shape[:2], np.uint8)
    kind[near(LAND_FILL)] = 1
    kind[near(WATER_FILL)] = 2
    return kind


def fit(array, fill):
    """Scale to WIDTH x HEIGHT with nearest sampling, cropping or padding evenly to keep the aspect."""
    height, width = array.shape
    scale = max(WIDTH / width, HEIGHT / height)
    scaled = np.asarray(Image.fromarray(array).resize((round(width * scale), round(height * scale)), Image.NEAREST))
    out = np.full((HEIGHT, WIDTH), fill, array.dtype)
    sy, sx = (scaled.shape[0] - HEIGHT) // 2, (scaled.shape[1] - WIDTH) // 2
    out[:, :] = scaled[sy:sy + HEIGHT, sx:sx + WIDTH]
    return out, scale, sx, sy


def distinct_colors(count, seed=7):
    rng = np.random.default_rng(seed)
    colors, used = [], {(0, 0, 0), (255, 255, 255)}
    golden = 0.618033988749895
    hue = rng.random()
    while len(colors) < count:
        hue = (hue + golden) % 1.0
        sat = 0.45 + 0.4 * rng.random()
        val = 0.55 + 0.4 * rng.random()
        color = tuple(int(c * 255) for c in colorsys.hsv_to_rgb(hue, sat, val))
        if color not in used:
            used.add(color)
            colors.append(color)
    return colors


def apply_overrides(keeper, path):
    """Move the land piece under (x, y) into the region under (to_x, to_y); coordinates are in output pixels."""
    if not path.exists():
        return
    with open(path, encoding='utf-8') as file:
        rows = [row for row in csv.DictReader(line for line in file if not line.startswith('#'))]
    for row in rows:
        x, y, tx, ty = (int(row[k]) for k in ('x', 'y', 'to_x', 'to_y'))
        source, target = keeper[y, x], keeper[ty, tx]
        if source <= 0 or target <= 0:
            raise ValueError(f'override {row}: both points must be on land')
        pieces, _ = ndimage.label(keeper == source, FOUR)
        piece = pieces == pieces[y, x]
        keeper[piece] = target
        print(f'override: {int(piece.sum())} px at ({x}, {y}) moved to the region at ({tx}, {ty}) {row.get("note", "")}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--out', type=Path, default=Path('build/import'))
    parser.add_argument('--overrides', type=Path, default=Path('source/region_overrides.csv'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    rgb = np.asarray(Image.open(args.source).convert('RGB'))
    kind = classify(rgb)
    print(f'source {rgb.shape[1]}x{rgb.shape[0]}: line {100 * (kind == 0).mean():.2f} %, land {100 * (kind == 1).mean():.2f} %, water {100 * (kind == 2).mean():.2f} %')

    # Regions are 4-connected land fills; water components touching the edge are sea, the rest lakes.
    region, region_count = ndimage.label(kind == 1, FOUR)
    water, water_count = ndimage.label(kind == 2, FOUR)
    edge = np.unique(np.concatenate([water[0], water[-1], water[:, 0], water[:, -1]]))
    edge = edge[edge != 0]
    areas = np.bincount(region.ravel())
    small = np.nonzero(areas < MIN_FILL)[0]
    small = small[small != 0]
    print(f'{region_count} land fills, {len(small)} specks under {MIN_FILL} px dropped, {water_count} water bodies, {len(edge)} open sea')

    # Every pixel gets the label of the nearest "keeper": big land fills, sea, lakes.
    keeper = np.zeros(kind.shape, np.int32)
    big = (region > 0) & ~np.isin(region, small)
    keeper[big] = region[big]
    sea_mask = np.isin(water, edge) & (water > 0)
    keeper[sea_mask] = -1
    keeper[(water > 0) & ~sea_mask] = -2
    _, (iy, ix) = ndimage.distance_transform_edt(keeper == 0, return_indices=True)
    keeper = keeper[iy, ix]

    keeper, scale, sx, sy = fit(keeper, -1)
    print(f'scaled x{scale:.4f}, cropped {sx} px per side horizontally, {sy} px vertically')

    # Regions below MIN_REGION (islets, slivers) join, as a whole, the large region closest to any of their pixels.
    sizes = np.bincount(np.maximum(keeper, 0).ravel())
    small_ids = [i for i in np.nonzero(sizes)[0] if i > 0 and sizes[i] < MIN_REGION]
    large = (keeper > 0) & (sizes[np.maximum(keeper, 0)] >= MIN_REGION)
    distance, (iy, ix) = ndimage.distance_transform_edt(~large, return_indices=True)
    source = np.where(np.isin(keeper, small_ids), keeper, 0)
    for index, box in zip(range(1, source.max() + 1), ndimage.find_objects(source)):
        if box is None:
            continue
        own = source[box] == index
        local = np.where(own, distance[box], np.inf)
        y, x = np.unravel_index(np.argmin(local), local.shape)
        target = keeper[iy[box][y, x], ix[box][y, x]]
        keeper[box][own] = target
    print(f'{len(small_ids)} regions under {MIN_REGION} px merged into the nearest large region')
    apply_overrides(keeper, args.overrides)

    classes = np.full(keeper.shape, LAND, np.uint8)
    classes[keeper == -1] = SEA
    classes[keeper < -1] = LAKE

    # Water pockets cut off by border lines: touching the sea -> sea, tiny and enclosed -> land.
    lakes, lake_count = ndimage.label(classes == LAKE, FOUR)
    near_sea = ndimage.binary_dilation(classes == SEA, FOUR)
    for index, box in enumerate(ndimage.find_objects(lakes), 1):
        own = lakes[box] == index
        if near_sea[box][own].any():
            classes[box][own] = SEA
            keeper[box][own] = -1
        elif own.sum() < MIN_FILL:
            classes[box][own] = LAND
    _, (iy, ix) = ndimage.distance_transform_edt(keeper <= 0, return_indices=True)
    keeper = np.where(classes == LAND, keeper[iy, ix], keeper)
    palette = np.zeros((4, 3), np.uint8)
    for color, value in PALETTE.items():
        palette[value] = color
    Image.fromarray(palette[classes]).save(args.out / 'land.png', optimize=True)

    # Renumber surviving regions 1..N by position (top-left to bottom-right) for stable ids.
    ids = np.unique(keeper[keeper > 0])
    objects = ndimage.find_objects(np.where(keeper > 0, keeper, 0))
    centers = ndimage.center_of_mass(keeper > 0, keeper, ids)
    order = sorted(range(len(ids)), key=lambda i: (round(centers[i][0] / 200), centers[i][1]))
    remap = np.zeros(keeper.max() + 1, np.int32)
    for new, i in enumerate(order, 1):
        remap[ids[i]] = new
    regions = np.where(keeper > 0, remap[np.maximum(keeper, 0)], 0)
    parts = np.bincount(ndimage.label(regions > 0, FOUR)[0].ravel())
    colors = distinct_colors(len(ids))
    image = np.zeros(regions.shape + (3,), np.uint8)
    lut = np.array([(0, 0, 0)] + colors, np.uint8)
    image[:] = lut[regions]
    Image.fromarray(image).save(args.out / 'regions.png', optimize=True)

    areas = np.bincount(regions.ravel(), minlength=len(ids) + 1)
    centers = ndimage.center_of_mass(regions > 0, regions, range(1, len(ids) + 1))
    with open(args.out / 'regions.csv', 'w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file)
        writer.writerow(['id', 'color', 'name', 'parent', 'type', 'provinces', 'area', 'x', 'y', 'pieces'])
        for index in range(1, len(ids) + 1):
            pieces = ndimage.label(regions == index, FOUR)[1]
            color = '#%02X%02X%02X' % colors[index - 1]
            cy, cx = centers[index - 1]
            writer.writerow([index, color, '', '', 'land', '', int(areas[index]), round(cx), round(cy), pieces])

    preview = Image.fromarray(np.where(classes[..., None] == SEA, np.uint8([40, 60, 110]), np.where(classes[..., None] == LAKE, np.uint8([70, 150, 230]), image)))
    preview = preview.resize((WIDTH // 2, HEIGHT // 2), Image.NEAREST)
    draw = ImageDraw.Draw(preview)
    for index in range(1, len(ids) + 1):
        cy, cx = centers[index - 1]
        text = str(index)
        x, y = cx / 2, cy / 2
        draw.text((x - 3 * len(text), y - 5), text, fill=(0, 0, 0), stroke_width=2, stroke_fill=(255, 255, 255))
    preview.save(args.out / 'regions_preview.png')
    lakes = ndimage.label(classes == LAKE, FOUR)[1]
    print(f'{len(ids)} regions, {lakes} lakes -> {args.out}')


if __name__ == '__main__':
    main()
