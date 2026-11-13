"""Validate land.png (one pixel per block, made by heightmap.py): exact palette, connectivity and thin features."""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

WIDTH, HEIGHT = 1152, 576   # blocks; a block is 4x4 px of the map
MIN_ISLAND = 4              # blocks
MIN_LAKE = 156              # blocks; smaller lakes are decorative (2500 px of the map)
MIN_THIN_CLUSTER = 5

SEA, LAND, LAKE, WASTE = 0, 1, 2, 3
PALETTE = {(0, 0, 0): SEA, (255, 255, 255): LAND, (0, 128, 255): LAKE, (128, 128, 128): WASTE}
NAMES = {SEA: 'sea', LAND: 'land', LAKE: 'lake', WASTE: 'wasteland'}
FOUR = ndimage.generate_binary_structure(2, 1)
EIGHT = ndimage.generate_binary_structure(2, 2)


class Report:
    def __init__(self):
        self.lines = []
        self.errors = 0
        self.warnings = 0
        self.marks = []

    def section(self, title):
        self.lines.append('')
        self.lines.append('== ' + title)

    def info(self, text):
        self.lines.append('   ' + text)

    def error(self, text, where=None):
        self.errors += 1
        self.lines.append(' E ' + text)
        if where is not None:
            self.marks.append((where, (255, 0, 0)))

    def warn(self, text, where=None):
        self.warnings += 1
        self.lines.append(' W ' + text)
        if where is not None:
            self.marks.append((where, (255, 160, 0)))


def load(path, report):
    image = Image.open(path)
    rgba = np.asarray(image.convert('RGBA'))
    report.info(f'{path.name}: mode {image.mode}, {image.width}x{image.height}')
    if (image.width, image.height) != (WIDTH, HEIGHT):
        report.error(f'size must be {WIDTH}x{HEIGHT}')
    if (rgba[..., 3] != 255).any():
        report.error('alpha channel is not fully opaque')
    rgb = rgba[..., :3].astype(np.uint32)
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    classes = np.full(key.shape, 255, np.uint8)
    for color, value in PALETTE.items():
        classes[key == ((color[0] << 16) | (color[1] << 8) | color[2])] = value
    stray = classes == 255
    if stray.any():
        ys, xs = np.nonzero(stray)
        report.error(f'{stray.sum()} pixels outside the palette, first at ({xs[0]}, {ys[0]})', (xs[0], ys[0]))
    return classes


def components(mask, structure):
    labels, count = ndimage.label(mask, structure)
    if count == 0:
        return labels, np.zeros(0, np.int64), []
    areas = np.bincount(labels.ravel())[1:]
    boxes = ndimage.find_objects(labels)
    return labels, areas, boxes


def center(box):
    return ((box[1].start + box[1].stop) // 2, (box[0].start + box[0].stop) // 2)


def check_summary(classes, report):
    report.section('Classes')
    total = classes.size
    for value, name in NAMES.items():
        count = int((classes == value).sum())
        report.info(f'{name:10} {count:10} blocks  {100 * count / total:6.2f} %')


def check_edges(classes, report):
    report.section('Map edges (land may reach them, as in EU4)')
    solid = classes != SEA
    ys, xs = np.nonzero(solid)
    report.info(f'non-sea bounding box x {xs.min()}..{xs.max()}, y {ys.min()}..{ys.max()}')
    report.info(f'closest to edges: left {xs.min()}, right {WIDTH - 1 - xs.max()}, top {ys.min()}, bottom {HEIGHT - 1 - ys.max()}')


def check_sea(classes, report):
    report.section('Sea connectivity')
    labels, areas, boxes = components(classes == SEA, FOUR)
    edge = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))) - {0}
    report.info(f'{len(areas)} sea components, {len(edge)} touch the map edge')
    for index, area in enumerate(areas, 1):
        if index not in edge:
            report.error(f'enclosed sea of {area} blocks at {center(boxes[index - 1])} (lake?)', center(boxes[index - 1]))


def check_land(classes, report):
    report.section('Landmasses (land + wasteland, 4-connected)')
    solid = (classes == LAND) | (classes == WASTE)
    labels, areas, boxes = components(solid, FOUR)
    order = np.argsort(-areas)
    report.info(f'{len(areas)} landmasses')
    for rank, index in enumerate(order):
        area = int(areas[index])
        box = boxes[index]
        text = f'#{rank + 1:3} {area:9} blocks  center {center(box)}  box x {box[1].start}..{box[1].stop - 1} y {box[0].start}..{box[0].stop - 1}'
        if area < MIN_ISLAND:
            report.warn('tiny island ' + text, center(box))
        elif rank < 40:
            report.info(text)
    passable_labels, passable_areas, passable_boxes = components(classes == LAND, FOUR)
    split = []
    for index in range(len(areas)):
        inside = np.unique(passable_labels[boxes[index]][labels[boxes[index]] == index + 1])
        inside = inside[inside != 0]
        if len(inside) > 1:
            parts = sorted((int(passable_areas[i - 1]) for i in inside), reverse=True)
            split.append((index, parts))
    report.section('Landmasses split by wasteland into unconnected passable parts')
    if not split:
        report.info('none')
    for index, parts in split:
        report.warn(f'landmass at {center(boxes[index])}: parts {parts[:8]}{" ..." if len(parts) > 8 else ""}', center(boxes[index]))


def check_diagonals(classes, report):
    report.section('Diagonal-only contacts (2x2 checkerboard of solid and water)')
    solid = (classes == LAND) | (classes == WASTE)
    a, b, c, d = solid[:-1, :-1], solid[:-1, 1:], solid[1:, :-1], solid[1:, 1:]
    checker = (a & d & ~b & ~c) | (b & c & ~a & ~d)
    ys, xs = np.nonzero(checker)
    report.info(f'{len(xs)} spots')
    for x, y in list(zip(xs, ys))[:30]:
        report.warn(f'checkerboard at ({x}, {y})', (x, y))


def check_thin(classes, report, mask, name):
    opened = ndimage.binary_opening(mask, np.ones((3, 3), bool))
    thin = mask & ~opened
    labels, areas, boxes = components(thin, EIGHT)
    big = [(int(areas[i]), boxes[i]) for i in range(len(areas)) if areas[i] >= MIN_THIN_CLUSTER]
    report.section(f'Thin {name} (narrower than 3 blocks)')
    report.info(f'{int(thin.sum())} blocks in {len(areas)} clusters, {len(big)} clusters of {MIN_THIN_CLUSTER}+ blocks')
    for area, box in sorted(big, key=lambda item: -item[0])[:30]:
        report.warn(f'{area:5} blocks at {center(box)}', center(box))


def check_lakes(classes, report):
    report.section('Lakes')
    labels, areas, boxes = components(classes == LAKE, FOUR)
    near_sea = ndimage.binary_dilation(classes == SEA, FOUR)
    report.info(f'{len(areas)} lakes, {int((areas >= MIN_LAKE).sum())} large enough to be lake provinces')
    for index, area in enumerate(areas, 1):
        box = boxes[index - 1]
        where = center(box)
        if near_sea[box][labels[box] == index].any():
            report.warn(f'lake of {area} blocks at {where} touches the sea', where)
        if area < MIN_LAKE:
            report.info(f'decorative lake {area:6} blocks at {where}')
        else:
            report.info(f'lake province  {area:6} blocks at {where}')


def check_waste(classes, report):
    report.section('Wasteland')
    labels, areas, boxes = components(classes == WASTE, FOUR)
    near_sea = ndimage.binary_dilation((classes == SEA) | (classes == LAKE), FOUR)
    report.info(f'{len(areas)} wasteland components')
    for index, area in enumerate(areas, 1):
        box = boxes[index - 1]
        touches = near_sea[box][labels[box] == index].any()
        report.info(f'{area:7} blocks at {center(box)}{"  touches water" if touches else ""}')


def preview(classes, report, path, scale=1):
    colors = np.array([[20, 40, 90], [225, 220, 200], [60, 140, 230], [120, 120, 120]], np.uint8)
    image = Image.fromarray(colors[np.minimum(classes, 3)]).resize((WIDTH // scale, HEIGHT // scale), Image.NEAREST)
    draw = ImageDraw.Draw(image)
    for (x, y), color in report.marks:
        x, y = x / scale, y / scale
        draw.ellipse((x - 9, y - 9, x + 9, y + 9), outline=color, width=3)
    image.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('land', type=Path)
    parser.add_argument('--out', type=Path, default=Path('build/validate'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = Report()
    classes = load(args.land, report)
    check_summary(classes, report)
    check_edges(classes, report)
    check_sea(classes, report)
    check_land(classes, report)
    check_diagonals(classes, report)
    check_thin(classes, report, (classes == LAND) | (classes == WASTE), 'land')
    check_thin(classes, report, (classes == SEA) | (classes == LAKE), 'water')
    check_lakes(classes, report)
    check_waste(classes, report)
    report.section('Result')
    report.info(f'{report.errors} errors, {report.warnings} warnings')
    text = '\n'.join(report.lines).lstrip('\n') + '\n'
    (args.out / 'land_report.txt').write_text(text, encoding='utf-8')
    preview(classes, report, args.out / 'land_issues.png')
    sys.stdout.write(text)
    return 1 if report.errors else 0


if __name__ == '__main__':
    sys.exit(main())
