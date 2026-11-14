#include "render/shaders.h"

#include <cstring>
#include <d3dcompiler.h>

namespace mc::shaders
{
    // World space: x = map x, y = up, z = -map y; one unit = one map pixel. The sea's surface is y = 0; the blocks
    // stand on the map's block grid (docs/RENDER.md).
    const char* const map_source = R"hlsl(
cbuffer Frame : register(b0)
{
    row_major float4x4 view_proj;
    float3 eye;
    float  time;
    float2 map_size;
    float  pixel_angle;     // world size of one screen pixel at distance 1
    uint   map_mode;        // 0 terrain: no fill; otherwise the fills colour the land (EU4 formula)
    uint   hovered;
    float3 frame_pad;
};

Texture2D<uint>           province_ids  : register(t0);
StructuredBuffer<float4>  fills         : register(t1);   // rgb, a = how much the map mode colours the province
StructuredBuffer<uint>    selected      : register(t3);   // per province: 1 = selected (editor)
SamplerState              s_linear      : register(s0);

static const float3 GREYIFY = float3(0.212671, 0.715160, 0.072169);


float hash(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}

float value_noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + float2(1, 0)), c = hash(i + float2(0, 1)), d = hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Province colour sampled like EU4's GetProvinceColorSampled: four point lookups, the colours (never the ids)
// blended bilinearly, so fills have soft edges between provinces. Only filled taps give colour (an unfilled tap
// must not wash it out), and the fill is as strong as their share: next to an unfilled province (wasteland, water)
// it fades out over a pixel. (A full fill as soon as any tap is filled flickered along such edges: the share of the
// neighbour's tap there is a rounding error.) `hover` gets the hovered province's share, `pick` the selected
// provinces'.
float4 province_fill(float2 m, out float hover, out float pick)
{
    float2 p = m - 0.5;
    float2 base = floor(p);
    float2 f = p - base;
    int2 t = int2(base);
    int2 hi = int2(map_size) - 1;
    uint i00 = province_ids.Load(int3(clamp(t, 0, hi), 0));
    uint i10 = province_ids.Load(int3(clamp(t + int2(1, 0), 0, hi), 0));
    uint i01 = province_ids.Load(int3(clamp(t + int2(0, 1), 0, hi), 0));
    uint i11 = province_ids.Load(int3(clamp(t + int2(1, 1), 0, hi), 0));
    float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float4 c00 = fills[i00], c10 = fills[i10], c01 = fills[i01], c11 = fills[i11];
    float4 wa = w * float4(c00.a, c10.a, c01.a, c11.a);
    float total = dot(wa, 1.0);
    float3 rgb = (c00.rgb * wa.x + c10.rgb * wa.y + c01.rgb * wa.z + c11.rgb * wa.w) / max(total, 1e-5);
    float4 h = float4(i00 == hovered, i10 == hovered, i01 == hovered, i11 == hovered) * (hovered != 0);
    hover = dot(h, w);
    pick = dot(float4(selected[i00], selected[i10], selected[i01], selected[i11]), w);
    return float4(rgb, saturate(total));
}

// ----------------------------------------------------------------------------------------------------- terrain
// The voxel surface (render/terrain.cpp): runs of block faces. Levels count blocks up from the sea floor, the sea's
// top is level 1 at world height 0; a level is block_size world units (a block is a cube).

Texture2D<uint4>          blocks         : register(t4);    // per block: ground top, water top (0 = none), material, class
StructuredBuffer<uint2>   terrain_quads  : register(t7);
Texture2DArray<float4>    block_layers   : register(t8);
struct TextureInfo
{
    uint  first_layer;
    uint  frames;
    float frame_time;
    uint  tint;        // 0 none, 1 grass, 2 water
};
StructuredBuffer<TextureInfo> block_textures : register(t9);
struct Material
{
    uint  top;
    uint  side;
    uint  overlay;     // NO_TEXTURE: none
    uint  under;
    uint  deep;
    uint3 material_pad;
};
StructuredBuffer<Material> materials  : register(t10);
Texture2D<float4>          grass_tint : register(t11);       // per block, blended between biomes
Texture2D<float4>          water_tint : register(t12);
SamplerState               s_point    : register(s1);        // crisp texels, blended mips, wrapping

cbuffer Terrain : register(b2)
{
    uint2 block_grid;
    float block_size;     // map px per block side, world units per level
    uint  under_depth;    // blocks under the top one that show `under`
    uint  water_still;
    uint  water_flow;
    uint2 terrain_pad;
};

static const uint  NO_TEXTURE = 0xFFFFFFFF;
// FACE_SLOPE: a waterfall's top, water sloping from its source's level (the edge toward the source, a side Face) down
// to its own.
static const uint  FACE_TOP = 0, FACE_WEST = 1, FACE_EAST = 2, FACE_NORTH = 3, FACE_SOUTH = 4, FACE_SLOPE = 5;
static const int2  FACE_NORMAL[6] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1), int2(0, 0) };
// Minecraft's face shading: tops full, north and south faces 0.8, east and west 0.6.
static const float FACE_SHADE[6] = { 1.0, 0.6, 0.6, 0.8, 0.8, 1.0 };
// Smooth lighting: a face corner's light by how many of its three neighbours in front of the face are solid.
static const float AO_LIGHT[4] = { 0.5, 0.65, 0.82, 1.0 };
static const float3 FOG_COLOR = float3(0.70, 0.80, 0.94);   // also the clear colour (MapRenderer kClear)
static const float  FOG_WIDTH = 48.0;                       // map px of fog inside every map edge

float level_y(float level) { return (level - 1.0) * block_size; }

uint4 block_at(int2 b)
{
    return any(b < 0) || any(b >= int2(block_grid)) ? uint4(0, 0, 0, 0) : blocks.Load(int3(b, 0));
}

// The highest top (ground or water) round a map point, world height: the border strips lie on it.
float surface_y(float2 m)
{
    uint top = 1;
    [unroll] for (int i = 0; i < 4; ++i) {
        float2 p = m + float2((i & 1) ? 0.5 : -0.5, (i & 2) ? 0.5 : -0.5);
        uint4 b = block_at(int2(floor(p / block_size)));
        top = max(top, max(b.x, b.y));
    }
    return level_y(top);
}

struct TerrainOut
{
    float4 pos   : SV_Position;
    float2 map   : TEXCOORD0;    // map px
    float  level : TEXCOORD1;    // blocks up from the sea floor
    nointerpolation uint face : TEXCOORD2;
};

TerrainOut vs_terrain(uint vid : SV_VertexID)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    uint2 q = terrain_quads[vid / 6];
    float2 c = corners[vid % 6];
    uint face = (q.x >> 21) & 7;
    float2 b = float2(q.x & 0x7FF, (q.x >> 11) & 0x3FF);
    float length = q.y & 0x7FF;
    float low = (q.y >> 11) & 0xFF;
    float high = (q.y >> 19) & 0xFF;
    float2 m;
    float level = lerp(low, high, c.y);
    if (face == FACE_TOP) {
        m = b + float2(c.x * length, c.y);
        level = high;
    } else if (face == FACE_SLOPE) {
        m = b + c;
        uint source = (q.y >> 27) & 7;
        float toward = source == FACE_WEST ? 1.0 - c.x : source == FACE_EAST ? c.x
                     : source == FACE_NORTH ? 1.0 - c.y : c.y;   // 1 at the edge toward the source
        level = lerp(low, high, toward);
    } else if (face == FACE_WEST || face == FACE_EAST) {
        m = float2(b.x + (face == FACE_EAST ? 1.0 : 0.0), b.y + c.x * length);
    } else {
        m = float2(b.x + c.x * length, b.y + (face == FACE_SOUTH ? 1.0 : 0.0));
    }
    m *= block_size;
    TerrainOut o;
    o.pos = mul(float4(m.x, level_y(level), -m.y, 1.0), view_proj);
    o.map = m;
    o.level = level;
    o.face = face;
    return o;
}

float4 block_texture(uint index, float2 uv, float2 gx, float2 gy)
{
    TextureInfo t = block_textures[index];
    uint frame = t.frames > 1 ? (uint)(time / t.frame_time) % t.frames : 0;
    return block_layers.SampleGrad(s_point, float3(frac(uv), t.first_layer + frame), gx, gy);
}

float3 block_tint(uint index, float2 m)
{
    uint kind = block_textures[index].tint;
    float2 at = m / (float2(block_grid) * block_size);
    return kind == 1 ? grass_tint.SampleLevel(s_linear, at, 0).rgb
         : kind == 2 ? water_tint.SampleLevel(s_linear, at, 0).rgb
         : float3(1.0, 1.0, 1.0);
}

float corner_light(bool side1, bool side2, bool corner)
{
    return AO_LIGHT[(side1 && side2) ? 0 : 3 - (uint(side1) + uint(side2) + uint(corner))];
}

// Ambient occlusion of a top at `layer` (the layer of air above it) over the block `b`, f = place in the face.
float top_light(int2 b, int layer, float2 f)
{
    bool s[3][3];
    [unroll] for (int y = 0; y < 3; ++y) {
        [unroll] for (int x = 0; x < 3; ++x) {
            s[y][x] = (int)block_at(b + int2(x - 1, y - 1)).x > layer;
        }
    }
    float l00 = corner_light(s[1][0], s[0][1], s[0][0]);
    float l10 = corner_light(s[1][2], s[0][1], s[0][2]);
    float l01 = corner_light(s[1][0], s[2][1], s[2][0]);
    float l11 = corner_light(s[1][2], s[2][1], s[2][2]);
    return lerp(lerp(l00, l10, f.x), lerp(l01, l11, f.x), f.y);
}

// Ambient occlusion of a side whose air is the column `front` at `layer`; `along` runs along the face (map px axis),
// f.x = place along it, f.y = place up the block.
float side_light(int2 front, int2 along, int layer, float2 f)
{
    int g0 = (int)block_at(front - along).x;
    int g1 = (int)block_at(front).x;
    int g2 = (int)block_at(front + along).x;
    // Solid at a layer: the column's ground covers it.
    float l00 = corner_light(g0 > layer, g1 > layer - 1, g0 > layer - 1);
    float l10 = corner_light(g2 > layer, g1 > layer - 1, g2 > layer - 1);
    float l01 = corner_light(g0 > layer, g1 > layer + 1, g0 > layer + 1);
    float l11 = corner_light(g2 > layer, g1 > layer + 1, g2 > layer + 1);
    return lerp(lerp(l00, l10, f.x), lerp(l01, l11, f.x), f.y);
}

float4 ps_terrain(TerrainOut input) : SV_Target
{
    uint face = input.face;
    int2 n = FACE_NORMAL[face];
    float2 m = input.map;
    float2 inside = m - float2(n) * 0.5;   // half a map px into the column the face belongs to
    int2 owner = int2(floor(inside / block_size));
    uint4 blk = block_at(owner);
    uint ground = blk.x;
    uint water = blk.y;
    uint top = max(ground, water);
    uint fall = blk.w >> 4;   // a waterfall: the Face toward its source
    if (fall != 0) {
        uint4 src = block_at(owner + FACE_NORMAL[fall]);
        uint source_top = max(src.x, src.y);
        // Its sides reach up to the source's water; cut them off above the slope.
        float2 f = saturate(m / block_size - float2(owner));
        float toward = fall == FACE_WEST ? 1.0 - f.x : fall == FACE_EAST ? f.x : fall == FACE_NORTH ? 1.0 - f.y : f.y;
        if (face != FACE_SLOPE) {
            clip(lerp((float)top, (float)source_top, toward) - input.level + 0.001);
        }
        top = source_top;
    }
    bool flat = face == FACE_TOP || face == FACE_SLOPE;
    int layer = flat ? (int)top - 1 : min((int)floor(input.level), (int)top - 1);
    bool wet = face == FACE_SLOPE || (face == FACE_TOP ? water > ground : layer >= (int)ground);

    // Texture coordinates in blocks (one texture per block), continuous across the run for the mip choice; sides
    // read left to right seen from outside, the texture's top row at the top of each block.
    float2 uv;
    if (face == FACE_TOP) {
        uv = m / block_size;
    } else if (face == FACE_SLOPE) {
        float2 down = -float2(FACE_NORMAL[fall]);   // the way the water flows, away from its source
        uv = float2(dot(m, float2(-down.y, down.x)), dot(m, down)) / block_size;
    } else {
        float along = face == FACE_EAST ? -m.y : face == FACE_WEST ? m.y : face == FACE_NORTH ? -m.x : m.x;
        uv = float2(along / block_size, -input.level);
    }
    float2 gx = ddx(uv);
    float2 gy = ddy(uv);

    float3 base;
    float light = 1.0;
    if (wet) {
        uint tex = face == FACE_TOP ? water_still : water_flow;
        base = block_texture(tex, uv, gx, gy).rgb * block_tint(tex, inside);
    } else {
        Material mat = materials[blk.z];
        uint tex = mat.top;
        uint overlay = NO_TEXTURE;
        if (face != FACE_TOP) {
            int depth = (int)ground - 1 - layer;
            tex = depth == 0 ? mat.side : depth <= (int)under_depth ? mat.under : mat.deep;
            overlay = depth == 0 ? mat.overlay : NO_TEXTURE;
        }
        base = block_texture(tex, uv, gx, gy).rgb * block_tint(tex, inside);
        if (overlay != NO_TEXTURE) {
            float4 o = block_texture(overlay, uv, gx, gy);
            base = lerp(base, o.rgb * block_tint(overlay, inside), o.a);
        }
        float2 f = frac(inside / block_size);
        if (face == FACE_TOP) {
            light = top_light(owner, layer + 1, f);
        } else {
            int2 along = int2(abs(n.y), abs(n.x));
            light = side_light(owner + n, along, layer, float2(dot(f, float2(along)), input.level - layer));
        }
    }

    // Map mode: EU4's political shading over dry faces (FRAME.md), the grey of the surface under the province
    // colour, pulled halfway to a constant.
    // Tops take the soft-edged sample; a side takes its own column's province exactly: it stands on the province edge,
    // where the blended sample would catch the neighbour's colour at a weight of nothing (any filled tap colours a
    // pixel) and flicker with every rounding of the position.
    float hover, pick;
    float4 fill;
    if (face == FACE_TOP || face == FACE_SLOPE) {
        fill = province_fill(inside, hover, pick);
    } else {
        uint pid = province_ids.Load(int3(owner * (int)block_size + (int)(block_size * 0.5), 0));
        fill = fills[pid];
        hover = (hovered != 0 && pid == hovered) ? 1.0 : 0.0;
        pick = (float)selected[pid];
    }
    float dry = wet ? 0.0 : 1.0;
    fill.a *= dry;
    float3 color = base;
    if (map_mode != 0) {
        float grey = lerp(0.36, dot(base, GREYIFY), 0.5);
        float3 political = 0.4 * grey + 0.45 * fill.rgb;
        color = lerp(base, political * 1.35, fill.a);
    }
    color *= FACE_SHADE[face] * light;
    color = lerp(color, color * 1.2 + 0.06, hover * 0.6);
    // Selected (editor): brighter, with light diagonal hatching 12 map px apart, so it reads apart from the hover.
    float hatch = step(0.5, frac((m.x + m.y) / 12.0));
    color = lerp(color, color * 1.25 + float3(0.10, 0.09, 0.05) + hatch * 0.10, pick * dry * 0.8);

    float edge = min(min(m.x, m.y), min(map_size.x - m.x, map_size.y - m.y));
    color = lerp(color, FOG_COLOR, 1.0 - smoothstep(0.0, FOG_WIDTH, edge));
    return float4(color, 1.0);
}

// ---------------------------------------------------------------------------------------------------- borders

struct BorderVertex
{
    float2 pos;      // map px on the smoothed polyline
    float2 normal;
    float  along;    // map px along the chain
    float  side;     // -1 or +1
    uint   chain;
    uint   provinces;   // the province on the -normal side | the one on the +normal side << 16
};
StructuredBuffer<BorderVertex> border_vertices : register(t6);
StructuredBuffer<uint>         border_styles   : register(t5);

// Styles: 1 province, 2 country (owners differ; regions without game data), 3 between two sea provinces.
static const float  STYLE_HALF_WIDTH[4] = { 0.0, 0.28, 1.3, 0.7 };    // map px
static const float  STYLE_MIN_PX[4]     = { 0.0, 0.8, 2.5, 1.0 };     // screen px, lines never get thinner
static const float3 STYLE_COLOR[4]      = { float3(0, 0, 0), float3(0.09, 0.08, 0.07), float3(0.05, 0.05, 0.08),
                                            float3(0.85, 0.92, 1.0) };
static const float  STYLE_ALPHA[4]      = { 0.0, 0.5, 0.85, 0.14 };

struct BorderOut
{
    float4 pos    : SV_Position;
    float  across : TEXCOORD0;   // map px from the centre line
    float  half_width : TEXCOORD1;
    float  pixel  : TEXCOORD2;   // map px per screen px here
    nointerpolation uint style : TEXCOORD3;
    float  along  : TEXCOORD4;   // map px along the chain
    nointerpolation float3 minus_color : TEXCOORD5;   // country colours of the two sides (country borders)
    nointerpolation float3 plus_color  : TEXCOORD6;
};

// The colour a side's country gives its half of a country border: the map mode's province colour, set even where the
// mode does not fill the land (terrain: a = 0), else a dark stone.
float3 side_color(uint province)
{
    float3 c = fills[province].rgb;
    return any(c > 0.0) ? c : float3(0.14, 0.13, 0.12);
}

// Drawn in two passes: country borders first (stencil 2), then the rest (stencil 1). A pixel is blended only by a
// pass of higher rank than what is there, so province lines end under a country line instead of crossing it.
cbuffer BorderDraw : register(b1)
{
    uint border_pass;
    float3 border_pad;
};

BorderOut vs_border(uint vid : SV_VertexID)
{
    BorderVertex v = border_vertices[vid];
    BorderOut o;
    o.style = border_styles[v.chain];
    if (o.style == 0 || (border_pass == 0) != (o.style == 2)) {
        o.pos = float4(0, 0, -1, 1);   // outside the depth range: the whole triangle is clipped
        o.across = o.half_width = o.pixel = o.along = 0.0;
        o.minus_color = o.plus_color = float3(0, 0, 0);
        return o;
    }
    float y = surface_y(v.pos);   // on the blocks: the top of the higher side
    float3 centre = float3(v.pos.x, y, -v.pos.y);
    float ps = distance(eye, centre) * pixel_angle;
    float hw = max(STYLE_HALF_WIDTH[o.style], STYLE_MIN_PX[o.style] * 0.5 * ps);
    float extent = hw + ps;   // one more screen pixel for antialiasing
    float2 m = v.pos + v.normal * v.side * extent;
    float3 world = float3(m.x, y, -m.y);
    world += normalize(eye - world) * (ps * 1.5 + 0.05);   // pulled a little toward the camera, above the map
    o.pos = mul(float4(world, 1.0), view_proj);
    o.across = v.side * extent;
    o.half_width = hw;
    o.pixel = ps;
    o.along = v.along;
    o.minus_color = side_color(v.provinces & 0xFFFF);
    o.plus_color = side_color(v.provinces >> 16);
    return o;
}

// EU4's country border (map/terrain/border5.dds with border5data.dds, gfx/FX/border.shader): the diffuse is black,
// the data texture adds the side's country colour (COLOR_TINT) times ~0.71 over a strip. Across one half, from the
// centre out (t = distance / half width): a clear gap, a black line, the colour strip with soft edges, a black edge.
float4 country_profile(float t, float3 color, float grain)
{
    if (t < 0.094 || t > 1.0) {
        return float4(0, 0, 0, 0);
    }
    float k = t < 0.19 ? 0.0
            : t < 0.30 ? lerp(0.30, 0.71, (t - 0.19) / 0.11)
            : t < 0.77 ? 0.71
            : t < 0.90 ? lerp(0.69, 0.37, (t - 0.77) / 0.13)
            : 0.0;
    return float4(color * k * grain, 1.0);
}

float4 ps_border(BorderOut input) : SV_Target
{
    if (input.style == 2) {
        // Four samples across the pixel: when zoomed out the profile averages into a dark line in both countries'
        // colours, as in EU4. The strip's colour varies a little along the border (EU4's data texture does too).
        float4 sum = 0.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            float across = input.across + (i - 1.5) * 0.25 * input.pixel;
            float side = across < 0.0 ? 0.0 : 1.0;
            float grain = 0.9 + 0.18 * value_noise(float2(input.along * 0.4, side * 17.0));
            float4 s = country_profile(abs(across) / input.half_width,
                                       across < 0.0 ? input.minus_color : input.plus_color, grain);
            sum += float4(s.rgb * s.a, s.a);
        }
        clip(sum.a - 0.01);
        return float4(sum.rgb / sum.a, sum.a * 0.25);
    }
    float coverage = saturate((input.half_width - abs(input.across)) / input.pixel + 0.5);
    float alpha = coverage * STYLE_ALPHA[input.style];
    if (input.style == 1) {
        alpha *= lerp(1.0, 0.35, saturate((input.pixel - 1.0) / 1.5));   // province lines recede when zoomed out
    }
    clip(alpha - 0.01);
    return float4(STYLE_COLOR[input.style], alpha);
}

// ----------------------------------------------------------------------------------------------------- labels
// Map names (core/labels.cpp places them, render/labels.cpp lays the glyphs on the map): an SDF atlas
// (tools/fonts/mapfont.py) drawn like archeologyEU4/Labels/LabelPython/rendering/text.py: all halos, then all inks.

struct LabelVertex
{
    float3 world;
    float2 texel;           // atlas texels
    float4 cell;            // the glyph's atlas cell x0, y0, x1, y1
    float  map_per_texel;
    float  sigma;           // halo Gaussian, map px
    uint   group;           // the name's key, 0 = none
    float  pad;
};
StructuredBuffer<LabelVertex> label_vertices : register(t16);
Texture2D<float>              label_atlas    : register(t17);
StructuredBuffer<uint>        label_keys     : register(t18);   // per province: the key its names are grouped by

cbuffer LabelDraw : register(b1)
{
    float2 label_atlas_size;
    float  label_spread;    // atlas texels of distance across the 8-bit range
    float  label_alpha;     // the layer's opacity (country names fade out, province names in, when zooming)
};

static const float3 LABEL_INK = float3(32.0, 30.0, 26.0) / 255.0;
static const float3 LABEL_HALO = float3(180.0, 180.0, 180.0) / 255.0;
static const float  LABEL_HALO_GAIN = 1.65;           // text.py HALO_GAIN: min(199, round(v * 1.65))
static const float  LABEL_HALO_MAX = 199.0 / 255.0;

struct LabelOut
{
    float4 pos   : SV_Position;
    float2 texel : TEXCOORD0;
    nointerpolation float4 cell : TEXCOORD1;
    nointerpolation float map_per_texel : TEXCOORD2;
    nointerpolation float sigma : TEXCOORD3;
    float3 world : TEXCOORD4;
    nointerpolation uint group : TEXCOORD5;
};

LabelOut vs_label(uint vid : SV_VertexID)
{
    LabelVertex v = label_vertices[vid];
    LabelOut o;
    o.pos = mul(float4(v.world, 1.0), view_proj);
    o.texel = v.texel;
    o.cell = v.cell;
    o.map_per_texel = v.map_per_texel;
    o.sigma = v.sigma;
    o.world = v.world;
    o.group = v.group;
    return o;
}

// Whether blocks hide a point of a name from the eye: a march along the ray from the point to the eye over the blocks
// it crosses (2D DDA in blocks) until the ray is above every block. A block hides the point if its top (ground or
// water) is above the ray where the ray enters it and more than LABEL_CLEARANCE levels above the name's plane, and
// its province is not of the name's own group: the name's own land never cuts it (it is what the name is about),
// other land standing in front of it does.
static const float LABEL_CLEARANCE = 2.0;   // levels
static const float LABEL_RAY_TOP = 40.0;    // level above every block

bool label_hidden(float3 p, uint group)
{
    float plane = p.y / block_size + 1.0;                   // the name's level
    float3 d = eye - p;
    float rise = d.y / block_size;                          // levels over the whole ray
    if (rise <= 0.0) {
        return false;
    }
    float t_end = min(1.0, (LABEL_RAY_TOP - plane) / rise);
    float2 m0 = float2(p.x, -p.z) / block_size;             // blocks
    float2 dm = float2(d.x, -d.z) / block_size;
    int2 cell = int2(floor(m0));
    int2 step_dir = int2(dm.x >= 0.0 ? 1 : -1, dm.y >= 0.0 ? 1 : -1);
    float2 inv = 1.0 / max(abs(dm), 1e-6);
    float2 next = float2(dm.x >= 0.0 ? floor(m0.x) + 1.0 - m0.x : m0.x - floor(m0.x),
                         dm.y >= 0.0 ? floor(m0.y) + 1.0 - m0.y : m0.y - floor(m0.y)) * inv;
    float t = 0.0;
    [loop] for (int i = 0; i < 128 && t < t_end; ++i) {
        uint4 b = block_at(cell);
        float top = (float)max(b.x, b.y);
        if (top > plane + LABEL_CLEARANCE && top > plane + rise * t) {
            uint province = province_ids.Load(int3(cell * (int)block_size + (int)(block_size * 0.5), 0));
            if (group == 0 || label_keys[province] != group) {
                return true;
            }
        }
        if (next.x < next.y) {
            t = next.x;
            next.x += inv.x;
            cell.x += step_dir.x;
        } else {
            t = next.y;
            next.y += inv.y;
            cell.y += step_dir.y;
        }
    }
    return false;
}

// Signed distance to the ink in map px (positive outside). Beyond the glyph's cell the distance continues from its
// edge, so the halo round a small name is not cut at the cell.
float label_distance(LabelOut input)
{
    float2 inside = clamp(input.texel, input.cell.xy + 0.5, input.cell.zw - 0.5);
    float sdf = label_atlas.Sample(s_linear, inside / label_atlas_size);
    return ((0.5 - sdf) * 2.0 * label_spread + distance(input.texel, inside)) * input.map_per_texel;
}

float4 ps_label_halo(LabelOut input) : SV_Target
{
    // The Gaussian-blurred ink at distance d from its edge is the normal CDF of -d / sigma (logistic approximation).
    float d = label_distance(input);
    float blurred = 1.0 / (1.0 + exp(1.702 * d / input.sigma));
    clip(blurred * LABEL_HALO_GAIN - 0.002);
    if (label_hidden(input.world, input.group)) {
        discard;
    }
    return float4(LABEL_HALO, min(LABEL_HALO_GAIN * blurred, LABEL_HALO_MAX) * label_alpha);
}

float4 ps_label_ink(LabelOut input) : SV_Target
{
    float d = label_distance(input);
    float coverage = saturate(0.5 - d / max(fwidth(d), 1e-4));
    clip(coverage - 0.002);
    if (label_hidden(input.world, input.group)) {
        discard;
    }
    return float4(LABEL_INK, coverage * label_alpha);
}
)hlsl";

    bool compile(const char* source, const char* name, const char* entry, const char* target, ID3D10Blob** blob,
                 std::string* error)
    {
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
        if (errors) {
            if (FAILED(hr) && error) {
                error->assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            }
            errors->Release();
        }
        return SUCCEEDED(hr);
    }
}
