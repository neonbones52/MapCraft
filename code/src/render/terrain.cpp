#include "render/terrain.h"

#include <algorithm>
#include <array>
#include <format>

#include "core/map_data.h"
#include "render/shaders.h"

namespace mc
{
    namespace
    {
        // Faces, as the shader numbers them (HLSL FACE_*): the top, then the sides by the way they face on the map.
        enum Face : std::uint32_t
        {
            kTop = 0,
            kWest = 1,    // -x
            kEast = 2,    // +x
            kNorth = 3,   // -map y
            kSouth = 4,   // +map y
            kSlope = 5,   // a waterfall's top: water sloping from its source's level down to its own
        };

        struct Side
        {
            Face face;
            int dx;
            int dy;
        };
        // Indexed by Face - 1.
        constexpr Side kSides[4] = { { kWest, -1, 0 }, { kEast, 1, 0 }, { kNorth, 0, -1 }, { kSouth, 0, 1 } };

        // HLSL TerrainQuad: a run of faces. a = x | y << 11 | face << 21 (the run's first block), b = length | low
        // level << 11 | high level << 19 | source << 27. A top covers `length` blocks along x at the high level; a side
        // lies on the block edge it faces, `length` blocks along that edge, from the low level to the high one; a
        // slope is one block, high at the edge toward its source (a Face), low at the opposite edge.
        struct Quad
        {
            std::uint32_t a;
            std::uint32_t b;
        };
        static_assert(sizeof(Quad) == 8);

        struct TerrainConstants
        {
            std::uint32_t blocks[2];     // block grid
            float block_size;            // map px per block side (= world units per level)
            std::uint32_t under_depth;
            std::uint32_t water_still;
            std::uint32_t water_flow;
            std::uint32_t pad[2];
        };
        static_assert(sizeof(TerrainConstants) % 16 == 0);

        constexpr std::uint32_t kMips = 5;   // 16, 8, 4, 2, 1

        bool check(HRESULT hr, const char* what, std::string* error)
        {
            if (FAILED(hr) && error) {
                *error = std::format("{} failed: 0x{:08X}", what, static_cast<std::uint32_t>(hr));
            }
            return SUCCEEDED(hr);
        }

        template <class T>
        bool structured(ID3D11Device* device, const std::vector<T>& data, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv,
                        const char* what, std::string* error)
        {
            std::vector<T> padded = data;
            padded.resize(std::max<std::size_t>(padded.size(), 1));
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(padded.size() * sizeof(T));
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(T);
            const D3D11_SUBRESOURCE_DATA init{ padded.data(), 0, 0 };
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            return check(device->CreateBuffer(&desc, &init, &buffer), what, error) &&
                   check(device->CreateShaderResourceView(buffer.Get(), nullptr, srv->ReleaseAndGetAddressOf()), what,
                         error);
        }

        bool texture2d(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, const void* data, UINT pitch,
                       Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA init{ data, pitch, 0 };
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            return check(device->CreateTexture2D(&desc, &init, &tex), what, error) &&
                   check(device->CreateShaderResourceView(tex.Get(), nullptr, srv->ReleaseAndGetAddressOf()), what, error);
        }

        // The next mip of a square RGBA8 image: 2 x 2 boxes, colours weighted by alpha so cut-out edges keep their
        // colour.
        std::vector<std::uint8_t> half(const std::uint8_t* src, std::uint32_t size)
        {
            const std::uint32_t n = size / 2;
            std::vector<std::uint8_t> out(std::size_t{ n } * n * 4);
            for (std::uint32_t y = 0; y < n; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    std::uint32_t sum[3] = {};
                    std::uint32_t alpha = 0;
                    for (std::uint32_t k = 0; k < 4; ++k) {
                        const std::uint8_t* p = src + ((std::size_t{ y } * 2 + k / 2) * size + x * 2 + k % 2) * 4;
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += p[c] * (p[3] + 1u);
                        }
                        alpha += p[3];
                    }
                    std::uint8_t* q = &out[(std::size_t{ y } * n + x) * 4];
                    const std::uint32_t weight = alpha + 4;
                    for (int c = 0; c < 3; ++c) {
                        q[c] = static_cast<std::uint8_t>(sum[c] / weight);
                    }
                    q[3] = static_cast<std::uint8_t>(alpha / 4);
                }
            }
            return out;
        }
    }

    bool TerrainLayer::init(ID3D11Device* device, const MapData& map, const TerrainLook& look, std::string* error)
    {
        ComPtr<ID3D10Blob> vs;
        ComPtr<ID3D10Blob> ps;
        const std::vector<std::uint8_t> falls = waterfalls(map);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;   // Minecraft: crisp texels, blended mips
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        return shaders::compile(shaders::map_source, "map.hlsl", "vs_terrain", "vs_5_0", &vs, error) &&
               shaders::compile(shaders::map_source, "map.hlsl", "ps_terrain", "ps_5_0", &ps, error) &&
               check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_),
                     "vs_terrain", error) &&
               check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_),
                     "ps_terrain", error) &&
               check(device->CreateSamplerState(&sd, &point_), "terrain sampler", error) &&
               create_quads(device, map, falls, error) && create_textures(device, map, look, falls, error);
    }

    std::vector<std::uint8_t> TerrainLayer::waterfalls(const MapData& map)
    {
        // Water next to a higher river or lake gets the higher water flowing down into it: the Face toward the
        // highest such neighbour (its source), 0 = none. Sea and lakes take falls from rivers (mouths) too.
        const auto w = static_cast<int>(map.block_width);
        const auto h = static_cast<int>(map.block_height);
        std::vector<std::uint8_t> falls(static_cast<std::size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (map.block_water[i] <= map.block_ground[i]) {
                    continue;
                }
                std::uint8_t best = map.block_water[i];
                for (const Side side : kSides) {
                    const int nx = x + side.dx;
                    const int ny = y + side.dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
                    const std::size_t j = static_cast<std::size_t>(ny) * w + nx;
                    const BlockClass cls = map.block_class[j];
                    if ((cls == BlockClass::river || cls == BlockClass::lake) &&
                        map.block_water[j] > map.block_ground[j] && map.block_water[j] > best) {
                        best = map.block_water[j];
                        falls[i] = static_cast<std::uint8_t>(side.face);
                    }
                }
            }
        }
        return falls;
    }

    bool TerrainLayer::create_quads(ID3D11Device* device, const MapData& map, const std::vector<std::uint8_t>& falls,
                                    std::string* error)
    {
        const auto w = static_cast<int>(map.block_width);
        const auto h = static_cast<int>(map.block_height);
        auto index = [&](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
        // The top of a column as its neighbours see it: its water if any (opaque, so it hides the bed), else its
        // ground. Outside the map the sea's top stands, so the land at the edge shows its sides down to the sea.
        auto top = [&](int x, int y) -> std::uint32_t {
            if (x < 0 || y < 0 || x >= w || y >= h) {
                return 1;
            }
            const std::size_t i = index(x, y);
            return std::max(map.block_ground[i], map.block_water[i]);
        };
        // A waterfall's source: the neighbour its Face points to.
        auto source = [&](int x, int y, int* sx, int* sy) {
            const Side& side = kSides[falls[index(x, y)] - 1];
            *sx = x + side.dx;
            *sy = y + side.dy;
        };
        // The top of a column for its own sides: a waterfall reaches up to its source's water (the shader cuts the
        // sides off under the slope).
        auto own_top = [&](int x, int y) -> std::uint32_t {
            if (falls[index(x, y)] == 0) {
                return top(x, y);
            }
            int sx = 0;
            int sy = 0;
            source(x, y, &sx, &sy);
            return top(sx, sy);
        };
        auto water = [&](int x, int y) { return map.block_water[index(x, y)] > map.block_ground[index(x, y)]; };
        std::vector<Quad> quads;
        auto push = [&](int x, int y, Face face, int length, std::uint32_t low, std::uint32_t high,
                        std::uint32_t extra = 0) {
            quads.push_back({ static_cast<std::uint32_t>(x) | static_cast<std::uint32_t>(y) << 11 | face << 21,
                              static_cast<std::uint32_t>(length) | low << 11 | high << 19 | extra << 27 });
        };

        // Tops: runs along x of equal height and the same kind (water or not); a waterfall is a slope of its own,
        // from its source's water down to its own.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w;) {
                if (const std::uint8_t fall = falls[index(x, y)]; fall != 0) {
                    push(x, y, kSlope, 1, top(x, y), own_top(x, y), fall);
                    ++x;
                    continue;
                }
                const std::uint32_t t = top(x, y);
                const bool wet = water(x, y);
                int end = x + 1;
                while (end < w && falls[index(end, y)] == 0 && top(end, y) == t && water(end, y) == wet) {
                    ++end;
                }
                push(x, y, kTop, end - x, t, t);
                x = end;
            }
        }
        // Sides: where a column stands above its neighbour, from the neighbour's top to its own; runs along the edge
        // with the same two levels. A neighbour's waterfall counts at its own (lower) water: where its slope stands
        // higher, depth hides the side behind it; where it is lower (a slope tilted another way), the side closes
        // the gap.
        for (const Side side : kSides) {
            const bool along_y = side.dx != 0;   // east and west faces run north-south
            const int lines = along_y ? w : h;
            const int cells = along_y ? h : w;
            for (int line = 0; line < lines; ++line) {
                std::uint32_t run_low = 0;
                std::uint32_t run_high = 0;
                int run_start = -1;
                for (int cell = 0; cell <= cells; ++cell) {
                    std::uint32_t low = 0;
                    std::uint32_t high = 0;
                    if (cell < cells) {
                        const int x = along_y ? line : cell;
                        const int y = along_y ? cell : line;
                        high = own_top(x, y);
                        low = top(x + side.dx, y + side.dy);
                        if (high <= low) {
                            high = low = 0;
                        }
                    }
                    if (run_start >= 0 && (high != run_high || low != run_low)) {
                        const int x = along_y ? line : run_start;
                        const int y = along_y ? run_start : line;
                        push(x, y, side.face, cell - run_start, run_low, run_high);
                        run_start = -1;
                    }
                    if (run_start < 0 && high > low) {
                        run_start = cell;
                        run_low = low;
                        run_high = high;
                    }
                }
            }
        }
        quad_count_ = static_cast<std::uint32_t>(quads.size());
        return structured(device, quads, &quads_, "terrain quads", error);
    }

    bool TerrainLayer::create_textures(ID3D11Device* device, const MapData& map, const TerrainLook& look,
                                       const std::vector<std::uint8_t>& falls, std::string* error)
    {
        const std::size_t blocks = std::size_t{ map.block_width } * map.block_height;
        if (look.block_material.size() != blocks || look.grass_tint.size() != blocks ||
            look.water_tint.size() != blocks || look.layer_count == 0 ||
            look.layers.size() != std::size_t{ look.layer_count } * TerrainLook::kTextureSize *
                                      TerrainLook::kTextureSize * 4) {
            if (error) {
                *error = "terrain look does not match the map's block grid";
            }
            return false;
        }
        std::vector<std::uint32_t> data(blocks);
        for (std::size_t i = 0; i < blocks; ++i) {
            data[i] = map.block_ground[i] | (map.block_water[i] << 8) | (look.block_material[i] << 16) |
                      (static_cast<std::uint32_t>(map.block_class[i]) << 24) |
                      (static_cast<std::uint32_t>(falls[i]) << 28);
        }

        // The block textures with their mips: subresource = mip + layer * kMips.
        std::vector<std::vector<std::uint8_t>> mips;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        mips.reserve(std::size_t{ look.layer_count } * kMips);
        constexpr std::uint32_t size = TerrainLook::kTextureSize;
        for (std::uint32_t layer = 0; layer < look.layer_count; ++layer) {
            const std::uint8_t* base = &look.layers[std::size_t{ layer } * size * size * 4];
            mips.emplace_back(base, base + std::size_t{ size } * size * 4);
            for (std::uint32_t m = 1, s = size; m < kMips; ++m, s /= 2) {
                mips.push_back(half(mips.back().data(), s));
            }
        }
        for (std::uint32_t layer = 0; layer < look.layer_count; ++layer) {
            for (std::uint32_t m = 0; m < kMips; ++m) {
                init.push_back({ mips[std::size_t{ layer } * kMips + m].data(), (size >> m) * 4, 0 });
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = kMips;
        desc.ArraySize = look.layer_count;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> array;

        const TerrainConstants constants{ { map.block_width, map.block_height },
                                          static_cast<float>(map.block_size),
                                          TerrainLook::kUnderDepth,
                                          look.water_still,
                                          look.water_flow,
                                          {} };
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(constants);
        cb.Usage = D3D11_USAGE_IMMUTABLE;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA cinit{ &constants, 0, 0 };

        const UINT bw = map.block_width;
        return check(device->CreateTexture2D(&desc, init.data(), &array), "block textures", error) &&
               check(device->CreateShaderResourceView(array.Get(), nullptr, &array_), "block textures", error) &&
               check(device->CreateBuffer(&cb, &cinit, &constants_), "terrain constants", error) &&
               texture2d(device, bw, map.block_height, DXGI_FORMAT_R8G8B8A8_UINT, data.data(), bw * 4, &blocks_,
                         "block data", error) &&
               texture2d(device, bw, map.block_height, DXGI_FORMAT_R8G8B8A8_UNORM, look.grass_tint.data(), bw * 4,
                         &grass_tint_, "grass tint", error) &&
               texture2d(device, bw, map.block_height, DXGI_FORMAT_R8G8B8A8_UNORM, look.water_tint.data(), bw * 4,
                         &water_tint_, "water tint", error) &&
               structured(device, look.textures, &textures_, "texture table", error) &&
               structured(device, look.materials, &materials_, "material table", error);
    }

    void TerrainLayer::draw(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* vs_srv[] = { blocks_.Get(), nullptr, nullptr, quads_.Get() };
        context->VSSetShaderResources(4, 4, vs_srv);
        ID3D11ShaderResourceView* ps_srv[] = { blocks_.Get(),     nullptr,         nullptr,
                                               quads_.Get(),      array_.Get(),    textures_.Get(),
                                               materials_.Get(),  grass_tint_.Get(), water_tint_.Get() };
        context->PSSetShaderResources(4, 9, ps_srv);
        ID3D11Buffer* cbs[] = { constants_.Get() };
        context->VSSetConstantBuffers(2, 1, cbs);
        context->PSSetConstantBuffers(2, 1, cbs);
        ID3D11SamplerState* samplers[] = { point_.Get() };
        context->PSSetSamplers(1, 1, samplers);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        context->Draw(quad_count_ * 6, 0);
    }
}
