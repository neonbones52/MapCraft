"""Pack the map sources into one little-endian container for the engine (layout: docs/MAPFORMAT.md).

    python tools/mapbake/bake.py        # -> build/map/world.mcmap
"""
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'mapgen'))
from generate_provinces import read_classes  # noqa: E402
from sources import province_map, read_csv  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools' / 'mapbake'))
import borders  # noqa: E402
import rivers  # noqa: E402

MAGIC = b'MCMP'
VERSION = 1
TYPES = {'land': 1, 'sea': 2, 'lake': 3}


def strings(values):
    blob = bytearray()
    offsets = [0]
    for value in values:
        blob += value.encode('utf-8')
        offsets.append(len(blob))
    return np.array(offsets, np.uint32).tobytes() + bytes(blob)


def number(value, default=np.nan):
    return float(value) if value not in ('', None) else default


def main():
    ids, provinces = province_map()
    count = max(provinces) + 1                      # index 0 = "no province"
    classes = read_classes(ROOT / 'land.png')
    regions = read_csv(ROOT / 'regions.csv')
    positions = {int(r['id']): r for r in read_csv(ROOT / 'positions.csv')}
    region_index = {int(r['id']): i + 1 for i, r in enumerate(regions)}

    kind = np.zeros(count, np.uint8)
    region = np.zeros(count, np.uint16)
    color = np.zeros(count, np.uint32)
    names = [''] * count
    pos = np.full((count, 8), np.nan, np.float32)   # city xy, unit xy, text xy, port xy
    port_sea = np.zeros(count, np.uint16)
    port_rot = np.zeros(count, np.float32)
    for pid, row in provinces.items():
        kind[pid] = TYPES[row['type']]
        region[pid] = region_index.get(int(row['region'] or 0), 0)
        c = int(row['color'].lstrip('#'), 16)
        color[pid] = ((c >> 16) & 255) | (((c >> 8) & 255) << 8) | ((c & 255) << 16) | (255 << 24)
        names[pid] = row['name']
        p = positions[pid]
        pos[pid] = [number(p[k]) for k in ('city_x', 'city_y', 'unit_x', 'unit_y', 'text_x', 'text_y', 'port_x', 'port_y')]
        port_sea[pid] = int(p['port_sea'] or 0)
        port_rot[pid] = number(p['port_rotation'], 0.0)

    neighbours = [[] for _ in range(count)]
    for r in read_csv(ROOT / 'adjacency.csv'):
        a, b, length = int(r['a']), int(r['b']), int(r['border'])
        neighbours[a].append((b, length))
        neighbours[b].append((a, length))
    offsets = np.zeros(count + 1, np.uint32)
    for pid in range(count):
        offsets[pid + 1] = offsets[pid] + len(neighbours[pid])
    adj_id = np.array([n for lst in neighbours for n, _ in sorted(lst)], np.uint16)
    adj_len = np.array([min(l, 65535) for lst in neighbours for _, l in sorted(lst)], np.uint16)

    parents = sorted({r['parent'] for r in regions})
    region_parent = np.array([0] + [parents.index(r['parent']) + 1 for r in regions], np.uint8)

    water = np.zeros(count, bool)
    water[kind != 1] = True
    border_points, border_chains = borders.build(ids, water)

    sections = [
        (b'PIDM', ids.astype(np.uint16).tobytes()),               # u16[w*h] province id per pixel
        (b'CLAS', classes.astype(np.uint8).tobytes()),            # u8[w*h] 0 sea, 1 land, 2 lake, 3 wasteland
        (b'PTYP', kind.tobytes()),                                # u8[n] 0 none, 1 land, 2 sea, 3 lake
        (b'PREG', region.tobytes()),                              # u16[n] region index (1-based, 0 none)
        (b'PCOL', color.tobytes()),                               # u32[n] debug colour RGBA8
        (b'PPOS', pos.tobytes()),                                 # f32[n*8] city, unit, text, port (map px, NaN = none)
        (b'PPSE', port_sea.tobytes()),                            # u16[n] sea province of the port
        (b'PPRO', port_rot.tobytes()),                            # f32[n] port facing, radians, x right / y down
        (b'ADJO', offsets.tobytes()),                             # u32[n+1] CSR offsets
        (b'ADJI', adj_id.tobytes()),                              # u16[m] neighbour ids
        (b'ADJL', adj_len.tobytes()),                             # u16[m] shared border length, px
        (b'PNAM', strings(names)),                                # u32[n+1] offsets + UTF-8
        (b'RNAM', strings([''] + [r['name'] for r in regions])),  # regions, 1-based
        (b'RPAR', region_parent.tobytes()),                       # u8[regions+1] parent index into LNAM
        (b'LNAM', strings([''] + parents)),                       # lands: the regions' parents
        (b'BVTX', border_points.tobytes()),                       # f32[m*2] border polyline points (map px)
        (b'BSEG', border_chains.tobytes()),                       # u32[k*4] first, count, a | b << 16, flags
    ]
    # Map label font (tools/fonts/mapfont.py): Mapfont.fnt-style metrics and an SDF atlas.
    font_file = ROOT / 'build' / 'fonts' / 'mapfont.npz'
    if not font_file.exists():
        sys.exit(f'{font_file} missing: run tools/fonts/mapfont.py')
    font = np.load(font_file)
    sections += [
        (b'FPAR', font['params'].astype(np.int32).tobytes()),     # i32[8] lineHeight, base, padding, atlas scale,
                                                                  #   extra texels, atlas w, h, SDF spread texels
        (b'FCHR', font['chars'].astype(np.int32).tobytes()),      # i32[c*12] code, atlas x, y, cell w, h, xoffset,
                                                                  #   yoffset, width, height, xadvance, quad x, y
        (b'FKRN', font['kerning'].astype(np.int32).tobytes()),    # i32[k*3] first, second, amount
        (b'FATL', font['atlas'].tobytes()),                       # u8[w*h] signed distance, 0.5 = ink edge
    ]
    # Province names, baked with the same placement (tools/labels/provinces.py).
    names_file = ROOT / 'build' / 'labels' / 'province_names.npz'
    if not names_file.exists():
        sys.exit(f'{names_file} missing: run tools/labels/provinces.py')
    sections.append((b'NGLY', np.load(names_file)['glyphs'].astype(np.float32).tobytes()))  # f32[g*7] x, y, dir x,
    #   dir y, quad width, height (map px), cp1252 code
    # Rivers (tools/mapbake/rivers.py); both sections empty without rivers.png.
    river_points, river_chains = [np.zeros((0, 3), np.float32)], []
    for river in rivers.traced():
        flags = (1 if river['source'] == 'land' else 0) | (2 if river['mouth'] == 'river' else 0) | \
            (4 if river['mouth'] in ('sea', 'lake') else 0)
        river_chains.append((sum(len(p) for p in river_points), len(river['points']), flags,
                             river['into'][0] if river['into'] else 0xFFFFFFFF))
        river_points.append(np.column_stack([river['points'], river['widths']]))
    sections += [
        (b'RVTX', np.vstack(river_points).astype(np.float32).tobytes()),   # f32[n*3] x, y, width (map px)
        (b'RSEG', np.array(river_chains, np.uint32).reshape(-1, 4).tobytes()),   # u32[r*4] first, count, flags, into
    ]
    tags = [tag for tag, _ in sections]
    duplicates = sorted({t for t in tags if tags.count(t) > 1})
    if duplicates:
        sys.exit(f'duplicate section tags {duplicates}')
    header = struct.pack('<4sIIIII', MAGIC, VERSION, ids.shape[1], ids.shape[0], count, len(sections))
    table_size = len(sections) * 16
    offset = len(header) + table_size
    table = bytearray()
    body = bytearray()
    for tag, data in sections:
        padding = (-(offset + len(body))) % 16
        body += b'\0' * padding
        table += struct.pack('<4sIQ', tag, len(data), offset + len(body))
        body += data
    out = ROOT / 'build' / 'map' / 'world.mcmap'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + bytes(table) + bytes(body))
    print(f'{out}: {out.stat().st_size / 1e6:.1f} MB, {count - 1} provinces, {len(adj_id) // 2} adjacencies, '
          f'{len(border_chains)} border chains, {len(river_chains)} rivers, {len(sections)} sections')


if __name__ == '__main__':
    main()
