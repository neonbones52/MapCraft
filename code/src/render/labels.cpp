#include "render/labels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include "core/map_data.h"
#include "render/shaders.h"

namespace mc
{
    namespace
    {
        struct LabelVertex
        {
            float world[3];
            float texel[2];     // atlas texels
            float cell[4];      // the glyph's atlas cell: x0, y0, x1, y1 (texels)
            float map_per_texel;
            float sigma;        // halo Gaussian, map px
            float pad[2];
        };
        static_assert(sizeof(LabelVertex) == 52);   // = the HLSL struct (structured buffer stride)

        struct DrawConstants
        {
            float atlas_size[2];
            float spread;
            float alpha;       // the whole layer's opacity (zoom fades)
        };

        bool check(HRESULT hr, const char* what, std::string* error)
        {
            if (FAILED(hr) && error) {
                *error = std::format("labels: {} failed: 0x{:08X}", what, static_cast<std::uint32_t>(hr));
            }
            return SUCCEEDED(hr);
        }
    }

    bool LabelLayer::init(ID3D11Device* device, const MapData& map, std::string* error)
    {
        ComPtr<ID3D10Blob> vs_blob, halo_blob, ink_blob;
        if (!shaders::compile(shaders::map_source, "map.hlsl", "vs_label", "vs_5_0", &vs_blob, error) ||
            !shaders::compile(shaders::map_source, "map.hlsl", "ps_label_halo", "ps_5_0", &halo_blob, error) ||
            !shaders::compile(shaders::map_source, "map.hlsl", "ps_label_ink", "ps_5_0", &ink_blob, error) ||
            !check(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_),
                   "vs", error) ||
            !check(device->CreatePixelShader(halo_blob->GetBufferPointer(), halo_blob->GetBufferSize(), nullptr,
                                             &halo_ps_), "ps", error) ||
            !check(device->CreatePixelShader(ink_blob->GetBufferPointer(), ink_blob->GetBufferSize(), nullptr,
                                             &ink_ps_), "ps", error)) {
            return false;
        }

        // The SDF atlas with a box-filtered mip chain (averaged distances keep minified names readable).
        const auto w = static_cast<std::uint32_t>(map.font_atlas_width);
        const auto h = static_cast<std::uint32_t>(map.font_atlas_height);
        std::vector<std::vector<std::uint8_t>> mips{ { map.font_atlas.begin(), map.font_atlas.end() } };
        for (std::uint32_t mw = w, mh = h; mw > 1 || mh > 1;) {
            const std::uint32_t nw = std::max(mw / 2, 1u);
            const std::uint32_t nh = std::max(mh / 2, 1u);
            const auto& src = mips.back();
            std::vector<std::uint8_t> dst(std::size_t(nw) * nh);
            for (std::uint32_t y = 0; y < nh; ++y) {
                for (std::uint32_t x = 0; x < nw; ++x) {
                    const std::uint32_t x0 = std::min(x * 2, mw - 1), x1 = std::min(x * 2 + 1, mw - 1);
                    const std::uint32_t y0 = std::min(y * 2, mh - 1), y1 = std::min(y * 2 + 1, mh - 1);
                    const std::uint32_t sum = src[std::size_t(y0) * mw + x0] + src[std::size_t(y0) * mw + x1] +
                                              src[std::size_t(y1) * mw + x0] + src[std::size_t(y1) * mw + x1];
                    dst[std::size_t(y) * nw + x] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
            mips.push_back(std::move(dst));
            mw = nw;
            mh = nh;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = static_cast<UINT>(mips.size());
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> init(mips.size());
        for (std::size_t i = 0; i < mips.size(); ++i) {
            init[i] = { mips[i].data(), std::max(w >> i, 1u), 0 };
        }
        ComPtr<ID3D11Texture2D> texture;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = sizeof(DrawConstants);
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const DrawConstants constants{ { float(w), float(h) }, float(map.font_spread), 1.0f };
        const D3D11_SUBRESOURCE_DATA cinit{ &constants, 0, 0 };
        atlas_size_[0] = float(w);
        atlas_size_[1] = float(h);
        spread_ = float(map.font_spread);
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        return check(device->CreateTexture2D(&desc, init.data(), &texture), "atlas", error) &&
               check(device->CreateShaderResourceView(texture.Get(), nullptr, &atlas_), "atlas view", error) &&
               check(device->CreateBuffer(&cd, &cinit, &constants_), "constants", error) &&
               check(device->CreateDepthStencilState(&ds, &no_depth_), "depth", error);
    }

    void LabelLayer::set_labels(ID3D11Device* device, const MapData& map, const LabelFont& font,
                                std::span<const MapLabel> labels)
    {
        std::vector<LabelVertex> vertices;
        const float s = static_cast<float>(map.font_atlas_scale);
        for (const MapLabel& label : labels) {
            for (const LabelGlyph& g : label.glyphs) {
                const LabelFont::Char& c = font.chars[g.code];
                if (!c.present || c.height <= 0 || g.width <= 0.0f || g.height <= 0.0f) {
                    continue;
                }
                const float hw = g.width * 0.5f;
                const float hh = g.height * 0.5f;
                const float texel_per_map = static_cast<float>(c.width) * s / g.width;
                // rendering/text.py: radius = max(0.3 map px, 0.055 x the glyph's height inside the padding).
                const float ink_height = static_cast<float>(c.height - 2 * font.padding) * g.height /
                                         static_cast<float>(c.height);
                const float sigma = std::max(0.3f, 0.055f * ink_height);
                const float margin = 3.0f * sigma;
                // Reading direction and "up" in map px (y down).
                const float dx = g.dir_x, dy = g.dir_y;
                const float ux = dy, uy = -dx;
                auto point = [&](float u, float v) { return std::pair{ g.x + dx * u + ux * v, g.y + dy * u + uy * v }; };
                const float cell[4] = { float(c.atlas_x), float(c.atlas_y), float(c.atlas_x + c.cell_width),
                                        float(c.atlas_y + c.cell_height) };
                LabelVertex corner[4];
                // The drawn quad covers the whole atlas cell (a glyph may reach beyond its layout quad, like Q's
                // tail) and the halo's reach round the layout quad.
                const float qx = static_cast<float>(c.quad_x);
                const float qy = static_cast<float>(c.quad_y);
                const float us[2] = { std::min(-hw - margin, -hw - qx / texel_per_map),
                                      std::max(hw + margin, -hw + (float(c.cell_width) - qx) / texel_per_map) };
                const float vs[2] = { std::min(-hh - margin, hh - (float(c.cell_height) - qy) / texel_per_map),
                                      std::max(hh + margin, hh + qy / texel_per_map) };
                for (int k = 0; k < 4; ++k) {
                    const float u = us[k & 1];
                    const float v = vs[k >> 1];
                    const auto [px, py] = point(u, v);
                    LabelVertex& vert = corner[k];
                    vert = {};
                    vert.world[0] = px;
                    vert.world[1] = 0.0f;   // the map plane (glyphs lie flat on it)
                    vert.world[2] = -py;
                    // The layout quad sits at (quad_x, quad_y) in the cell, its top edge up (v = +hh).
                    vert.texel[0] = float(c.atlas_x) + qx + (u + hw) * texel_per_map;
                    vert.texel[1] = float(c.atlas_y) + qy + (hh - v) * texel_per_map;
                    std::copy(cell, cell + 4, vert.cell);
                    vert.map_per_texel = 1.0f / texel_per_map;
                    vert.sigma = sigma;
                }
                for (const int k : { 0, 1, 2, 1, 3, 2 }) {
                    vertices.push_back(corner[k]);
                }
            }
        }
        glyph_count_ = vertices.size() / 6;
        vertices_.Reset();
        if (vertices.empty()) {
            return;
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(LabelVertex));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(LabelVertex);
        const D3D11_SUBRESOURCE_DATA init{ vertices.data(), 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, &init, &buffer)) ||
            FAILED(device->CreateShaderResourceView(buffer.Get(), nullptr, &vertices_))) {
            vertices_.Reset();
            glyph_count_ = 0;
        }
    }

    void LabelLayer::draw(ID3D11DeviceContext* context, float alpha)
    {
        if (!vertices_ || glyph_count_ == 0 || alpha <= 0.001f) {
            return;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const DrawConstants constants{ { atlas_size_[0], atlas_size_[1] }, spread_, alpha };
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(constants_.Get(), 0);
        }
        ID3D11ShaderResourceView* vs_srv[] = { vertices_.Get() };
        context->VSSetShaderResources(16, 1, vs_srv);
        ID3D11ShaderResourceView* ps_srv[] = { atlas_.Get() };
        context->PSSetShaderResources(17, 1, ps_srv);
        ID3D11Buffer* cbs[] = { constants_.Get() };
        context->PSSetConstantBuffers(1, 1, cbs);
        context->OMSetDepthStencilState(no_depth_.Get(), 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        const UINT count = static_cast<UINT>(glyph_count_ * 6);
        context->PSSetShader(halo_ps_.Get(), nullptr, 0);   // every halo first, so none washes out an ink
        context->Draw(count, 0);
        context->PSSetShader(ink_ps_.Get(), nullptr, 0);
        context->Draw(count, 0);
    }
}
