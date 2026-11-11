#include "render/shaders.h"

#include <cstring>
#include <d3dcompiler.h>

namespace mc::shaders
{
    // World space: x = map x, y = up, z = -map y; one unit = one map pixel. The map is the plane y = 0 (the voxel
    // surface that replaces it is still to come, docs/RENDER.md).
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
Texture2D<uint>           classes       : register(t2);   // pixel class: 0 sea, 1 land, 2 lake, 3 wasteland
StructuredBuffer<uint>    selected      : register(t3);   // per province: 1 = selected (editor)
SamplerState              s_linear      : register(s0);

static const float3 GREYIFY = float3(0.212671, 0.715160, 0.072169);

// Placeholder surface colours of the flat map, per pixel class.
static const float3 CLASS_COLOR[4] = { float3(0.13, 0.25, 0.42), float3(0.47, 0.55, 0.36), float3(0.20, 0.38, 0.55),
                                       float3(0.45, 0.43, 0.40) };
static const float3 WATER_COLOR = float3(0.20, 0.38, 0.55);   // rivers, like lakes

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

int2 texel(float2 m) { return clamp(int2(floor(m)), int2(0, 0), int2(map_size) - 1); }

// --------------------------------------------------------------------------------------------------------- map

struct MapOut
{
    float4 pos : SV_Position;
    float2 map : TEXCOORD0;
};

MapOut vs_map(uint vid : SV_VertexID)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    MapOut o;
    o.map = corners[vid] * map_size;
    o.pos = mul(float4(o.map.x, 0.0, -o.map.y, 1.0), view_proj);
    return o;
}

// Province colour sampled like EU4's GetProvinceColorSampled: four point lookups, the colours (never the ids)
// blended bilinearly, so fills have soft edges between provinces. Only filled taps give colour (a water tap next
// to land must not wash it out); where land ends is left to the caller. `hover` gets the hovered province's share,
// `pick` the selected provinces'.
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
    return float4(rgb, total > 0.0 ? 1.0 : 0.0);
}

float4 ps_map(MapOut input) : SV_Target
{
    float2 m = input.map;
    uint cls = min(classes.Load(int3(texel(m), 0)), 3u);
    float dry = (cls == 1 || cls == 3) ? 1.0 : 0.0;
    float3 material = CLASS_COLOR[cls];
    float hover, pick;
    float4 fill = province_fill(m, hover, pick);
    fill.a *= dry;
    float3 color = material;
    if (map_mode != 0) {
        // EU4 political shading: grey of the material under the province colour (FRAME.md), pulled halfway to a
        // constant.
        float grey = lerp(0.36, dot(material, GREYIFY), 0.5);
        float3 political = 0.4 * grey + 0.45 * fill.rgb;
        color = lerp(material, political * 1.35, fill.a);
    }
    color = lerp(color, color * 1.2 + 0.06, hover * 0.6);
    // Selected (editor): brighter, with light diagonal hatching 12 map px apart, so it reads apart from the hover.
    float hatch = step(0.5, frac((m.x + m.y) / 12.0));
    color = lerp(color, color * 1.25 + float3(0.10, 0.09, 0.05) + hatch * 0.10, pick * dry * 0.8);
    return float4(color, 1.0);
}

// ----------------------------------------------------------------------------------------------------- rivers
// EU4 draws rivers.bmp as strips; ours lie on the map plane in the water colour, never thinner than a screen pixel.

struct RiverVertex
{
    float2 pos;         // map px on the river's line
    float2 normal;      // to the left bank, looking downstream
    float  along;       // map px from the source
    float  side;        // -1 or +1
    float  half_width;  // map px
    float  pad;
};
StructuredBuffer<RiverVertex> river_vertices : register(t6);

struct RiverOut
{
    float4 pos        : SV_Position;
    float  across     : TEXCOORD0;   // map px from the centre line
    float  half_width : TEXCOORD1;
    float  pixel      : TEXCOORD2;   // map px per screen px here
};

RiverOut vs_river(uint vid : SV_VertexID)
{
    RiverVertex v = river_vertices[vid];
    RiverOut o;
    float3 centre = float3(v.pos.x, 0.0, -v.pos.y);
    float ps = distance(eye, centre) * pixel_angle;
    float hw = max(v.half_width, 0.6 * ps);   // never thinner than about a screen pixel
    float extent = hw + ps;                   // one more screen pixel for antialiasing
    float2 m = v.pos + v.normal * v.side * extent;
    float3 world = float3(m.x, 0.0, -m.y);
    world += normalize(eye - world) * (ps * 1.5 + 0.05);   // pulled a little toward the camera, above the map
    o.pos = mul(float4(world, 1.0), view_proj);
    o.across = v.side * extent;
    o.half_width = hw;
    o.pixel = ps;
    return o;
}

float4 ps_river(RiverOut input) : SV_Target
{
    float d = abs(input.across);
    float coverage = saturate((input.half_width - d) / input.pixel + 0.5);
    clip(coverage - 0.01);
    return float4(WATER_COLOR, 0.92 * coverage);
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
    float3 centre = float3(v.pos.x, 0.0, -v.pos.y);
    float ps = distance(eye, centre) * pixel_angle;
    float hw = max(STYLE_HALF_WIDTH[o.style], STYLE_MIN_PX[o.style] * 0.5 * ps);
    float extent = hw + ps;   // one more screen pixel for antialiasing
    float2 m = v.pos + v.normal * v.side * extent;
    float3 world = float3(m.x, 0.0, -m.y);
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
    float2 pad;
};
StructuredBuffer<LabelVertex> label_vertices : register(t16);
Texture2D<float>              label_atlas    : register(t17);

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
    return o;
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
    return float4(LABEL_HALO, min(LABEL_HALO_GAIN * blurred, LABEL_HALO_MAX) * label_alpha);
}

float4 ps_label_ink(LabelOut input) : SV_Target
{
    float d = label_distance(input);
    float coverage = saturate(0.5 - d / max(fwidth(d), 1e-4));
    clip(coverage - 0.002);
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
