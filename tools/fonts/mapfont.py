"""Map label font: EU4's Mapfont.fnt layout metrics and an SDF glyph atlas, made from Cinzel (the font in the project
root) instead of EU4's bitmap Trajan Pro Bold.

The label placement (code/src/core/labels.cpp, archeologyEU4/Labels) measures names with these metrics exactly as
EU4 measures them with gfx/fonts/Mapfont.fnt: per byte (cp1252) xadvance, glyph quad xoffset/yoffset/width/height
including an 8 px padding, kerning pairs, lineHeight 100. Mapfont is Trajan Pro Bold at size 100 with the capitals'
ink from 18 to 78 px below the line top (A: yoffset 10 + padding 8, height 76 - 16). Cinzel is scaled so that its
capitals take the same 60 px and sit on the same base line 78, so the placement's proportions (text height against
the territory's thickness, glyph quads, cross-section points) stay those of EU4. Weight 700: Mapfont is bold=1.

The placement centres every glyph's quad on its curve (EU4 CurveText). Cinzel's Q sweeps a long tail to the right and
below, so its quad centre would pull the letter's body off its place; the same holds, less, for Ç and J under the
base line. The metric quad is therefore the letter's LAYOUT box: Q takes O's box, Ç takes C's, and no box reaches
below the base line further than O's (round letters' overshoot). The atlas cell holds the whole glyph, tail included,
round the layout quad; FCHR gives the layout quad's place in the cell (quad_x, quad_y).

The atlas holds a signed distance field per glyph at ATLAS_SCALE x the metric size: the glyph's ink and its layout
quad, with the padding and EXTRA texels round them (the halo reaches beyond the quad). Only the bytes an upper-cased name can contain are kept:
EU4 upper-cases names byte by byte (StrToUpper) before measuring them. Letters, digits and NAME_PUNCTUATION.

    python tools/fonts/mapfont.py      # -> build/fonts/mapfont.npz + preview_mapfont.png
"""
import math
import sys
from pathlib import Path

import numpy as np
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / 'Cinzel-VariableFont_wght.ttf'
WEIGHT = 700
LINE_HEIGHT = 100          # Mapfont.fnt common lineHeight
BASE = 78                  # Mapfont.fnt common base
CAP_TOP = 18               # where Mapfont's capitals' ink begins below the line top
PADDING = 8                # Mapfont.fnt info padding (all four sides)
ATLAS_SCALE = 2            # atlas texels per metric px
EXTRA = 20                 # atlas texels beyond the metric quad on every side (halo room)
SPREAD = 32.0              # atlas texels of signed distance across the 8-bit range
SUPER = 4                  # the distance field is computed at SUPER x the atlas resolution
ATLAS_WIDTH = 2048
LAYOUT_OF = {'Q': 'O', '\u00c7': 'C'}   # letters placed by the box of their body
NAME_PUNCTUATION = " '-.,()&!?:/\"’"


def upper_bytes():
    """cp1252 bytes a name can hold after EU4's byte-wise StrToUpper (lowercase letters map away)."""
    keep = []
    for code in range(32, 256):
        try:
            ch = bytes([code]).decode('cp1252')
        except UnicodeDecodeError:
            continue
        up = ch.upper()
        if len(up) == 1 and up != ch:
            continue                      # a lowercase letter: never measured
        if not (ch.isalnum() or ch in NAME_PUNCTUATION):
            continue                      # symbols no place name uses (a missing glyph is skipped, as in EU4)
        keep.append(code)
    return keep


def kerning_pairs(font, glyph_of):
    """XAdvance adjustments of the GPOS 'kern' feature for the glyph pairs we use, font units."""
    gpos = font['GPOS'].table
    lookups = {i for fr in gpos.FeatureList.FeatureRecord if fr.FeatureTag == 'kern'
               for i in fr.Feature.LookupListIndex}
    wanted = set(glyph_of.values())
    pairs = {}
    for index in sorted(lookups):
        lookup = gpos.LookupList.Lookup[index]
        for sub in lookup.SubTable:
            if lookup.LookupType == 9:
                sub = sub.ExtSubTable
            if getattr(sub, 'LookupType', 2) != 2 and lookup.LookupType != 9:
                continue
            coverage = sub.Coverage.glyphs
            if sub.Format == 1:
                for first, pair_set in zip(coverage, sub.PairSet):
                    if first not in wanted:
                        continue
                    for record in pair_set.PairValueRecord:
                        value = getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0
                        if record.SecondGlyph in wanted and value and (first, record.SecondGlyph) not in pairs:
                            pairs[first, record.SecondGlyph] = value
            elif sub.Format == 2:
                class1, class2 = sub.ClassDef1.classDefs, sub.ClassDef2.classDefs
                for first in coverage:
                    if first not in wanted:
                        continue
                    row = sub.Class1Record[class1.get(first, 0)]
                    for second in wanted:
                        record = row.Class2Record[class2.get(second, 0)]
                        value = getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0
                        if value and (first, second) not in pairs:
                            pairs[first, second] = value
    return pairs


def signed_distance(mask):
    """Signed distance (px, positive outside the ink) of a boolean mask, measured between pixel centres."""
    outside = ndimage.distance_transform_edt(~mask)
    inside = ndimage.distance_transform_edt(mask)
    return np.where(mask, 0.5 - inside, outside - 0.5)


def main():
    font = instantiateVariableFont(TTFont(FONT), {'wght': WEIGHT})
    units = font['head'].unitsPerEm
    cap = font['OS/2'].sCapHeight
    k = (BASE - CAP_TOP) / cap                        # metric px per font unit
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    codes = [c for c in upper_bytes() if ord(bytes([c]).decode('cp1252')) in cmap]
    glyph_of = {c: cmap[ord(bytes([c]).decode('cp1252'))] for c in codes}

    raster = ImageFont.truetype(str(FONT), round(units * k * ATLAS_SCALE * SUPER))
    raster.set_variation_by_axes([WEIGHT])

    def ink_box(char):
        """left, top, right, bottom of a character's ink in metric px (top-down, base line at BASE), or None."""
        pen = BoundsPen(glyphs)
        glyphs[cmap[ord(char)]].draw(pen)
        if pen.bounds is None:
            return None
        x_min, y_min, x_max, y_max = pen.bounds
        return (math.floor(x_min * k), BASE - math.ceil(y_max * k), math.ceil(x_max * k), BASE - math.floor(y_min * k))

    lowest = ink_box('O')[3]                          # round capitals' overshoot below the base line
    chars, cells = [], []
    for code in codes:
        char = bytes([code]).decode('cp1252')
        advance = round(font['hmtx'][glyph_of[code]][0] * k)
        ink = ink_box(char)
        if ink is None:                                # blank (space): only an advance, like Mapfont's height 0
            chars.append([code, 0, 0, 0, 0, 0, 0, 0, 0, advance, 0, 0])
            continue
        # Layout box: the body's box (Q -> O), never below the round capitals' base line.
        body = ink_box(LAYOUT_OF[char]) if char in LAYOUT_OF else ink
        left, top, right = body[0], ink[1], body[2]
        bottom = min(body[3], lowest) if body[3] > lowest else body[3]
        if bottom <= top:
            bottom = top + 1
        xoffset, yoffset = left - PADDING, top - PADDING
        width, height = right - left + 2 * PADDING, bottom - top + 2 * PADDING
        # The atlas cell: ink and layout quad with padding, at ATLAS_SCALE, plus EXTRA texels round it.
        u_left, u_top = min(ink[0], left) - PADDING, min(ink[1], top) - PADDING
        u_right, u_bottom = max(ink[2], right) + PADDING, max(ink[3], bottom) + PADDING
        cw = (u_right - u_left) * ATLAS_SCALE + 2 * EXTRA
        ch = (u_bottom - u_top) * ATLAS_SCALE + 2 * EXTRA
        quad_x = (xoffset - u_left) * ATLAS_SCALE + EXTRA
        quad_y = (yoffset - u_top) * ATLAS_SCALE + EXTRA
        hi = Image.new('L', (cw * SUPER, ch * SUPER))
        # Pen origin (cursor 0, base line) in cell coordinates at SUPER x atlas resolution.
        ox = (-u_left * ATLAS_SCALE + EXTRA) * SUPER
        oy = ((BASE - u_top) * ATLAS_SCALE + EXTRA) * SUPER
        ImageDraw.Draw(hi).text((ox, oy), char, font=raster, fill=255, anchor='ls')
        distance = signed_distance(np.asarray(hi) >= 128) / SUPER          # atlas texels
        distance = distance.reshape(ch, SUPER, cw, SUPER).mean(axis=(1, 3))
        cells.append(np.clip(np.round((0.5 - distance / (2 * SPREAD)) * 255), 0, 255).astype(np.uint8))
        chars.append([code, 0, 0, cw, ch, xoffset, yoffset, width, height, advance, quad_x, quad_y])

    # Shelf packing, tallest first.
    order = sorted(range(len(cells)), key=lambda i: -cells[i].shape[0])
    x = y = shelf = 0
    places = {}
    for i in order:
        h, w = cells[i].shape
        if x + w > ATLAS_WIDTH:
            x, y, shelf = 0, y + shelf, 0
        places[i] = (x, y)
        x += w
        shelf = max(shelf, h)
    atlas_height = 1 << math.ceil(math.log2(y + shelf))
    atlas = np.zeros((atlas_height, ATLAS_WIDTH), np.uint8)
    cell_of = [i for i, row in enumerate(chars) if row[3]]
    for i, cell in enumerate(cells):
        cx, cy = places[i]
        atlas[cy:cy + cell.shape[0], cx:cx + cell.shape[1]] = cell
        chars[cell_of[i]][1:3] = [cx, cy]

    pairs = kerning_pairs(font, glyph_of)
    code_of = {g: c for c, g in glyph_of.items()}
    kerning = [[code_of[a], code_of[b], round(v * k)] for (a, b), v in sorted(pairs.items())
               if round(v * k) != 0]
    out = ROOT / 'build' / 'fonts'
    out.mkdir(parents=True, exist_ok=True)
    np.savez(out / 'mapfont.npz',
             params=np.array([LINE_HEIGHT, BASE, PADDING, ATLAS_SCALE, EXTRA, ATLAS_WIDTH, atlas_height,
                              int(SPREAD)], np.int32),
             chars=np.array(chars, np.int32), kerning=np.array(kerning, np.int32).reshape(-1, 3), atlas=atlas)
    Image.fromarray(atlas).save(out / 'preview_mapfont.png')
    # The same metrics as a BMFont text file like EU4's gfx/fonts/Mapfont.fnt (for the reference Python port).
    lines = [f'info face="Cinzel" size=100 bold=1 italic=0 padding={PADDING},{PADDING},{PADDING},{PADDING} spacing=1,1',
             f'common lineHeight={LINE_HEIGHT} base={BASE} scaleW={ATLAS_WIDTH} scaleH={atlas_height} pages=1']
    lines += [f'char id={c} x={x} y={y} width={w} height={h} xoffset={xo} yoffset={yo} xadvance={adv} page=0'
              for c, x, y, _, _, xo, yo, w, h, adv, _, _ in chars]
    lines += [f'kerning first={a} second={b} amount={v}' for a, b, v in kerning]
    (out / 'mapfont.fnt').write_text('\n'.join(lines) + '\n', encoding='cp1252')
    print(f'{out / "mapfont.npz"}: {len(chars)} characters ({len(cells)} glyphs), {len(kerning)} kerning pairs, '
          f'atlas {ATLAS_WIDTH}x{atlas_height}, Cinzel wght {WEIGHT} at {k * units:.1f} px/em')


if __name__ == '__main__':
    sys.exit(main())
