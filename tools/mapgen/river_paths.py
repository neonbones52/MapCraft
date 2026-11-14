"""rivers.png on the block grid: every river as a path of blocks from its source to its mouth, and what is wrong.

rivers.png, transparent except the river lines (EU4 rivers.bmp convention, one pixel per block, over the land only):
    (0, 255, 0)    source: the first block of a river
    (0, 225, 255)  brook, 1 block wide
    (0, 100, 255)  river, 2 blocks wide
    (255, 0, 0)    join: the last block of a tributary, next to the river it flows into
    (255, 252, 0)  split (not supported yet)
A line steps to its 4 neighbours, or diagonally where neither corner block is drawn. A river ends at its join, at the
first block whose side touches water (sea or lake), or at its last block when that touches water only by a corner; a
source touching a lake is that lake's outflow.
Rivers touch each other only at a join. The water runs downhill: a block more than CARVE steps above the lowest
block upstream would need a canyon and is reported.

    python tools/mapgen/river_paths.py      # check rivers.png -> build/rivers/rivers.png
"""
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

from heightmap import ROOT, STEPS, read_blocks
from validate_land import HEIGHT, LAKE, LAND, SEA, WIDTH

BROOK, RIVER, SOURCE, JOIN, SPLIT = 1, 2, 3, 4, 5
COLORS = {(0, 225, 255): BROOK, (0, 100, 255): RIVER, (0, 255, 0): SOURCE, (255, 0, 0): JOIN, (255, 252, 0): SPLIT}
CARVE = 2               # steps a river may cut below its banks where the ground rises (1 elsewhere)
JOIN_REACH = 2          # last blocks of a tributary allowed to touch the river it joins
FOUR_STEPS = ((0, 1), (1, 0), (0, -1), (-1, 0))
DIAGONALS = ((1, 1), (1, -1), (-1, 1), (-1, -1))
EIGHT_STEPS = FOUR_STEPS + DIAGONALS


def read_lines(path=ROOT / 'rivers.png'):
    """-> (line kind per block, 0 = none; problems that make the file unusable)."""
    rgba = np.asarray(Image.open(path).convert('RGBA')).astype(np.uint32)
    problems = []
    if rgba.shape[:2] != (HEIGHT, WIDTH):
        problems.append(f'size {rgba.shape[1]}x{rgba.shape[0]}, must be {WIDTH}x{HEIGHT}')
        return np.zeros((HEIGHT, WIDTH), np.uint8), problems
    key = (rgba[..., 0] << 16) | (rgba[..., 1] << 8) | rgba[..., 2]
    drawn = rgba[..., 3] >= 128
    kind = np.zeros(key.shape, np.uint8)
    for (r, g, b), value in COLORS.items():
        kind[drawn & (key == ((r << 16) | (g << 8) | b))] = value
    stray = drawn & (kind == 0)
    if stray.any():
        y, x = np.argwhere(stray)[0]
        problems.append(f'{int(stray.sum())} drawn pixels of other colours, first at ({x}, {y})')
    return kind, problems


def inside(y, x):
    return 0 <= y < HEIGHT and 0 <= x < WIDTH


def next_blocks(kind, y, x):
    """Line blocks a line steps to from (y, x): 4-neighbours, and diagonals whose corners are both undrawn."""
    out = [(y + dy, x + dx) for dy, dx in FOUR_STEPS if inside(y + dy, x + dx) and kind[y + dy, x + dx]]
    for dy, dx in DIAGONALS:
        if inside(y + dy, x + dx) and kind[y + dy, x + dx] and not kind[y + dy, x] and not kind[y, x + dx]:
            out.append((y + dy, x + dx))
    return out


def touches(mask, y, x):
    return any(inside(y + dy, x + dx) and mask[y + dy, x + dx] for dy, dx in EIGHT_STEPS)


def touches_side(mask, y, x):
    return any(inside(y + dy, x + dx) and mask[y + dy, x + dx] for dy, dx in FOUR_STEPS)


def trace(kind, classes):
    """-> (rivers, problems [(x, y, text)]). A river: dict(path [(y, x)], width [1 | 2] per block, source 'land' |
    'lake', mouth 'sea' | 'lake' | 'river' | None, into: river index it joins or None)."""
    water = (classes == SEA) | (classes == LAKE)
    lake = classes == LAKE
    problems = []
    rivers = []
    for sy, sx in sorted(map(tuple, np.argwhere(kind == SOURCE)), key=lambda p: (p[1], p[0])):
        path, mouth = [(sy, sx)], None
        while True:
            y, x = path[-1]
            if len(path) > 1 and kind[y, x] == JOIN:
                mouth = 'river'
                break
            ahead = [p for p in next_blocks(kind, y, x) if p not in path[-3:] and kind[p] != SOURCE]
            if len(path) > 1 and (touches_side(water, y, x) or (not ahead and touches(water, y, x))):
                mouth = 'lake' if touches(lake, y, x) and not touches(classes == SEA, y, x) else 'sea'
                break
            if len(ahead) > 1:
                ahead = [p for p in ahead if kind[p] != JOIN] or ahead
            if len(ahead) > 1:
                problems.append((x, y, 'the line forks here (a split or another river touching it)'))
                break
            if not ahead:
                problems.append((x, y, f'river from ({sx}, {sy}) ends on land'))
                break
            path.append(ahead[0])
        widths = []
        for p in path:
            widths.append(2 if kind[p] == RIVER else 1 if kind[p] == BROOK else widths[-1] if widths else 0)
        first = next((w for w in widths if w), 1)
        widths = [w or first for w in widths]
        source = 'lake' if touches(lake, sy, sx) else 'land'
        if touches(classes == SEA, sy, sx):
            problems.append((sx, sy, 'a source by the sea'))
        rivers.append(dict(path=path, width=widths, source=source, mouth=mouth, into=None))

    owner = np.full(kind.shape, -1, np.int32)
    for index, river in enumerate(rivers):
        for p in river['path']:
            if owner[p] >= 0 and owner[p] != index:
                problems.append((p[1], p[0], 'two rivers run through the same block'))
            owner[p] = index
    for index, river in enumerate(rivers):
        if river['mouth'] != 'river':
            continue
        y, x = river['path'][-1]
        targets = {owner[y + dy, x + dx] for dy, dx in EIGHT_STEPS
                   if inside(y + dy, x + dx) and owner[y + dy, x + dx] not in (-1, index)}
        if len(targets) != 1:
            problems.append((x, y, 'a join next to no other river' if not targets else 'a join next to two rivers'))
            river['mouth'] = None
        else:
            river['into'] = targets.pop()

    for y, x in np.argwhere((kind > 0) & (owner < 0)):
        problems.append((int(x), int(y), 'line not reached from any source'))
        break
    unreached = int(((kind > 0) & (owner < 0)).sum())
    if unreached:
        problems[-1] = (problems[-1][0], problems[-1][1], f'{unreached} line blocks not reached from any source, '
                                                          f'first here')
    for y, x in np.argwhere(kind == SPLIT):
        problems.append((int(x), int(y), 'a split: not supported yet'))
    off = (kind > 0) & (classes != LAND)
    if off.any():
        y, x = np.argwhere(off)[0]
        problems.append((int(x), int(y), f'{int(off.sum())} line blocks on water, first here'))

    for index, river in enumerate(rivers):
        tail = set(river['path'][-JOIN_REACH:]) if river['mouth'] == 'river' else set()
        for y, x in river['path']:
            if (y, x) in tail:
                continue
            others = [owner[y + dy, x + dx] for dy, dx in EIGHT_STEPS if inside(y + dy, x + dx)]
            other = next((o for o in others if o not in (-1, index) and not (
                rivers[o]['mouth'] == 'river' and rivers[o]['into'] == index)), None)
            if other is not None and index < other:
                problems.append((int(x), int(y), f'rivers from {fmt(rivers[index])} and {fmt(rivers[other])} touch'))
                break
    return rivers, problems


def fmt(river):
    y, x = river['path'][0]
    return f'({x}, {y})'


def uphill(rivers, steps):
    """-> [(x, y, river, rise)]: the first block of every stretch that stands more than CARVE steps above the lowest
    block upstream, with the largest rise in the stretch; and tributaries meeting a river higher than themselves."""
    found = []
    levels = []
    for river in rivers:
        level, run, rise = STEPS + 1, None, 0
        river_levels = []
        for y, x in river['path']:
            s = int(steps[y, x])
            if s > level + CARVE:
                if run is None:
                    run, rise = (x, y), 0
                rise = max(rise, s - level)
            elif run is not None:
                found.append((*run, fmt(river), rise))
                run = None
            level = min(level, s)
            river_levels.append(level)
        if run is not None:
            found.append((*run, fmt(river), rise))
        levels.append(river_levels)
    for index, river in enumerate(rivers):
        if river['into'] is None:
            continue
        target = rivers[river['into']]
        y, x = river['path'][-1]
        near = min(range(len(target['path'])), key=lambda i: abs(target['path'][i][0] - y) + abs(target['path'][i][1] - x))
        if levels[river['into']][near] > levels[index][-1] + CARVE:
            found.append((x, y, fmt(river), levels[river['into']][near] - levels[index][-1]))
    return found


def river_mask(rivers, steps):
    """-> bool per block: the blocks of every river path, plus, where a path steps diagonally, the lower of the two
    corner blocks, so the water is 4-connected."""
    mask = np.zeros(steps.shape, bool)
    for river in rivers:
        path = river['path']
        for (y0, x0), (y1, x1) in zip(path, path[1:]):
            if y0 != y1 and x0 != x1:
                a, b = (y0, x1), (y1, x0)
                mask[a if steps[a] <= steps[b] else b] = True
        for p in path:
            mask[p] = True
    return mask


def preview(classes, steps, rivers, marks, scale=2):
    rgb = np.empty(classes.shape + (3,), np.uint8)
    rgb[...] = (48 + steps.astype(np.int32) * 160 // STEPS)[..., None].astype(np.uint8)
    rgb[classes == SEA] = (20, 40, 90)
    rgb[classes == LAKE] = (60, 140, 230)
    for river in rivers:
        for (y, x), w in zip(river['path'], river['width']):
            rgb[y, x] = (0, 100, 255) if w == 2 else (0, 225, 255)
        y, x = river['path'][0]
        rgb[y, x] = (0, 255, 0)
    image = Image.fromarray(rgb).resize((WIDTH * scale, HEIGHT * scale), Image.NEAREST)
    draw = ImageDraw.Draw(image)
    for x, y, color in marks:
        draw.ellipse((scale * x - 9, scale * y - 9, scale * x + 9, scale * y + 9), outline=color, width=3)
    return image


def main():
    kind, problems = read_lines()
    if problems:
        sys.stderr.write('rivers.png: ' + '; '.join(problems) + '\n')
        return 1
    classes, steps, _ = read_blocks()
    rivers, problems = trace(kind, classes)
    climbs = uphill(rivers, steps)
    mouths = {}
    for river in rivers:
        mouths[river['mouth']] = mouths.get(river['mouth'], 0) + 1
    out = [f'{len(rivers)} rivers, {sum(len(r["path"]) for r in rivers)} blocks; '
           f'mouths: ' + ', '.join(f'{k or "none"} {v}' for k, v in sorted(mouths.items(), key=lambda i: str(i[0]))) +
           f'; {sum(r["source"] == "lake" for r in rivers)} flow out of lakes']
    out.append(f'{len(problems)} problems' + (':' if problems else ''))
    out += [f'  ({x}, {y}) {text}' for x, y, text in problems]
    out.append(f'{len(climbs)} uphill stretches (more than {CARVE} steps above the water upstream)' +
               (':' if climbs else ''))
    out += [f'  ({x}, {y}) river from {river}: up to {rise} steps' for x, y, river, rise in climbs]
    folder = ROOT / 'build' / 'rivers'
    folder.mkdir(parents=True, exist_ok=True)
    marks = [(x, y, (255, 0, 0)) for x, y, _ in problems] + [(x, y, (255, 160, 0)) for x, y, _, _ in climbs]
    preview(classes, steps, rivers, marks).save(folder / 'rivers.png')
    out.append('wrote build/rivers/rivers.png (red: problems, orange: uphill)')
    sys.stdout.write('\n'.join(out) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
