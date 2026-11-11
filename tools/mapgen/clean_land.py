"""Remove pixel noise from land.png without touching the source file."""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

from validate_land import FOUR, LAKE, LAND, PALETTE, SEA, WASTE

MAX_SPECK = 64


def classify(path):
    rgb = np.asarray(Image.open(path).convert('RGB')).astype(np.uint32)
    key = (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    classes = np.full(key.shape, 255, np.uint8)
    for color, value in PALETTE.items():
        classes[key == ((color[0] << 16) | (color[1] << 8) | color[2])] = value
    if (classes == 255).any():
        raise ValueError('pixels outside the palette; run validate_land.py first')
    return classes


def majority_neighbour(classes, mask):
    """Class of the most common 4-neighbour outside the mask for every labelled speck."""
    labels, count = ndimage.label(mask, FOUR)
    ring = ndimage.binary_dilation(mask, FOUR) & ~mask
    result = {}
    for index, box in enumerate(ndimage.find_objects(labels), 1):
        grown = (slice(max(box[0].start - 1, 0), box[0].stop + 1), (slice(max(box[1].start - 1, 0), box[1].stop + 1)))
        own = ndimage.binary_dilation(labels[grown] == index, FOUR) & ring[grown]
        values = np.bincount(classes[grown][own], minlength=4)
        result[index] = int(np.argmax(values))
    return labels, result


def absorb(classes, value, max_area, name, log):
    """Give every component of `value` smaller than max_area to its dominant neighbour class."""
    mask = classes == value
    labels, count = ndimage.label(mask, FOUR)
    areas = np.bincount(labels.ravel())
    small = np.isin(labels, np.nonzero(areas <= max_area)[0]) & mask
    if value == SEA:
        edge = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
        small &= ~np.isin(labels, edge)
    if not small.any():
        return 0
    specks, targets = majority_neighbour(classes, small)
    for index, target in targets.items():
        classes[specks == index] = target
    log.append(f'{name}: {len(targets)} specks, {int(small.sum())} px reassigned')
    return len(targets)


def break_checkerboards(classes, log):
    """Turn the water pixel of every solid/water 2x2 checkerboard into land."""
    total = 0
    while True:
        solid = (classes == LAND) | (classes == WASTE)
        a, b, c, d = solid[:-1, :-1], solid[:-1, 1:], solid[1:, :-1], solid[1:, 1:]
        main = a & d & ~b & ~c
        anti = b & c & ~a & ~d
        if not (main.any() or anti.any()):
            break
        ys, xs = np.nonzero(main)
        classes[ys, xs + 1] = LAND
        ys, xs = np.nonzero(anti)
        classes[ys, xs] = LAND
        total += int(main.sum() + anti.sum())
    if total:
        log.append(f'checkerboards: {total} water pixels turned into land')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('land', type=Path)
    parser.add_argument('--out', type=Path, default=Path('build/land_clean.png'))
    args = parser.parse_args()
    classes = classify(args.land)
    log = []
    break_checkerboards(classes, log)
    absorb(classes, SEA, MAX_SPECK, 'enclosed sea', log)
    absorb(classes, LAND, MAX_SPECK, 'land', log)
    absorb(classes, LAKE, MAX_SPECK, 'lake', log)
    absorb(classes, WASTE, MAX_SPECK, 'wasteland', log)
    palette = np.zeros((4, 3), np.uint8)
    for color, value in PALETTE.items():
        palette[value] = color
    args.out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(palette[classes]).save(args.out, optimize=True)
    changed = int((classify(args.land) != classes).sum())
    print('\n'.join(log) or 'nothing to clean')
    print(f'{changed} px changed -> {args.out}')


if __name__ == '__main__':
    main()
