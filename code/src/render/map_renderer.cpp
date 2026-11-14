#include "render/map_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <vector>

#include "core/map_data.h"
#include "render/shaders.h"

namespace mc
{
    namespace
    {
        struct FrameConstants
        {
            float view_proj[4][4];
            float eye[3];
            float time;
            float map_size[2];
            float pixel_angle;
            std::uint32_t map_mode;
            std::uint32_t hovered;
            float pad[3];
        };
        static_assert(sizeof(FrameConstants) % 16 == 0);

        struct BorderVertex
        {
            float pos[2];
            float normal[2];
            float along;
            float side;
            std::uint32_t chain;
            std::uint32_t provinces;   // province on the -normal side | the one on the +normal side << 16
        };
        static_assert(sizeof(BorderVertex) == 32);

        constexpr float kBorderStep = 4.0f;       // map px: border strips have a point at least this often
        constexpr float kClear[4] = { 0.70f, 0.80f, 0.94f, 1.0f };   // the fog at the map edges (HLSL FOG_COLOR)

        bool check(HRESULT hr, const char* what, std::string* error)
        {
            if (FAILED(hr) && error) {
                *error = std::format("{} failed: 0x{:08X}", what, static_cast<std::uint32_t>(hr));
            }
            return SUCCEEDED(hr);
        }

        template <class T>
        bool structured(ID3D11Device* device, std::size_t count, const T* data, bool dynamic,
                        Microsoft::WRL::ComPtr<ID3D11Buffer>* buffer, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv,
                        const char* what, std::string* error)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(std::max<std::size_t>(count, 1) * sizeof(T));
            desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(T);
            const D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
            Microsoft::WRL::ComPtr<ID3D11Buffer> local;
            Microsoft::WRL::ComPtr<ID3D11Buffer>* target = buffer ? buffer : std::addressof(local);
            return check(device->CreateBuffer(&desc, data ? &init : nullptr, target->ReleaseAndGetAddressOf()), what,
                         error) &&
                   check(device->CreateShaderResourceView(target->Get(), nullptr, srv->ReleaseAndGetAddressOf()), what,
                         error);
        }

        bool texture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, const void* data, UINT pitch,
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

        template <class T>
        void upload(ID3D11DeviceContext* context, ID3D11Buffer* buffer, std::span<const T> data)
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, data.data(), data.size_bytes());
                context->Unmap(buffer, 0);
            }
        }
    }

    bool MapRenderer::init(ID3D11Device* device, const MapData& map, const TerrainLook& look, std::string* error)
    {
        device_ = device;
        map_width_ = static_cast<float>(map.width);
        map_height_ = static_cast<float>(map.height);
        province_count_ = map.count;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(FrameConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!check(device->CreateBuffer(&cb, nullptr, &frame_constants_), "CreateBuffer(frame)", error)) {
            return false;
        }

        std::vector<ProvinceFill> no_fill(map.count);
        std::vector<std::uint32_t> no_selection(map.count, 0);
        return create_shaders(error) && create_borders(map, error) && create_states(error) &&
               terrain_.init(device, map, look, error) &&
               texture(device, map.width, map.height, DXGI_FORMAT_R16_UINT, map.pixel_province.data(), map.width * 2,
                       &province_ids_, "province ids", error) &&
               structured(device, no_fill.size(), no_fill.data(), true, &fills_buffer_, &fills_, "fills", error) &&
               structured(device, no_selection.size(), no_selection.data(), true, &selection_buffer_, &selection_,
                          "selection", error) &&
               labels_.init(device, map, error) && province_labels_.init(device, map, error);
    }

    bool MapRenderer::create_shaders(std::string* error)
    {
        auto vs = [&](const char* entry, ComPtr<ID3D11VertexShader>& out) {
            ComPtr<ID3D10Blob> blob;
            return shaders::compile(shaders::map_source, "map.hlsl", entry, "vs_5_0", &blob, error) &&
                   check(device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out),
                         entry, error);
        };
        auto ps = [&](const char* entry, ComPtr<ID3D11PixelShader>& out) {
            ComPtr<ID3D10Blob> blob;
            return shaders::compile(shaders::map_source, "map.hlsl", entry, "ps_5_0", &blob, error) &&
                   check(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out),
                         entry, error);
        };
        return vs("vs_border", border_vs_) && ps("ps_border", border_ps_);
    }

    bool MapRenderer::create_borders(const MapData& map, std::string* error)
    {
        // Two vertices per polyline point, one on each side; segments longer than kBorderStep are split, so the strip
        // follows the blocks under it.
        std::vector<BorderVertex> vertices;
        std::vector<std::uint32_t> indices;
        chain_count_ = static_cast<std::uint32_t>(map.border_chains.size() / 4);
        std::vector<float> xs;
        std::vector<float> ys;
        for (std::uint32_t c = 0; c < chain_count_; ++c) {
            const std::uint32_t first = map.border_chains[c * 4];
            const std::uint32_t count = map.border_chains[c * 4 + 1];
            const bool closed = (map.border_chains[c * 4 + 3] & 1) != 0;
            if (count < 2 || (first + count) * 2 > map.border_points.size()) {
                continue;
            }
            xs.clear();
            ys.clear();
            for (std::uint32_t i = 0; i < count; ++i) {
                const float x = map.border_points[(first + i) * 2];
                const float y = map.border_points[(first + i) * 2 + 1];
                if (i > 0) {
                    const float px = xs.back();
                    const float py = ys.back();
                    const int steps = static_cast<int>(std::ceil(std::hypot(x - px, y - py) / kBorderStep));
                    for (int s = 1; s < steps; ++s) {
                        const float t = static_cast<float>(s) / static_cast<float>(steps);
                        xs.push_back(px + (x - px) * t);
                        ys.push_back(py + (y - py) * t);
                    }
                }
                xs.push_back(x);
                ys.push_back(y);
            }
            const auto base = static_cast<std::uint32_t>(vertices.size());
            const std::size_t n = xs.size();
            // Which of the chain's two provinces lies on the +normal side: a vote of the pixels just off the line at
            // a quarter, half and three quarters of its length (country borders colour each side by its owner).
            const std::uint32_t pair = map.border_chains[c * 4 + 2];
            const std::uint32_t a = pair & 0xFFFF;
            const std::uint32_t b = pair >> 16;
            int votes_a = 0;
            for (const std::size_t i : { n / 4, n / 2, (3 * n) / 4 }) {
                const std::size_t i0 = std::min(i, n - 2);
                float nx = -(ys[i0 + 1] - ys[i0]);
                float ny = xs[i0 + 1] - xs[i0];
                const float nl = std::hypot(nx, ny);
                if (nl < 1e-6f) {
                    continue;
                }
                const float px = (xs[i0] + xs[i0 + 1]) * 0.5f + nx / nl * 0.7f;
                const float py = (ys[i0] + ys[i0 + 1]) * 0.5f + ny / nl * 0.7f;
                if (px >= 0.0f && py >= 0.0f && px < static_cast<float>(map.width) &&
                    py < static_cast<float>(map.height)) {
                    const std::uint32_t id = map.pixel_province[static_cast<std::size_t>(py) * map.width +
                                                                static_cast<std::size_t>(px)];
                    votes_a += id == a ? 1 : id == b ? -1 : 0;
                }
            }
            const std::uint32_t provinces = votes_a > 0 ? (b | (a << 16)) : (a | (b << 16));
            float along = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t prev = i > 0 ? i - 1 : (closed ? n - 2 : 0);
                std::size_t next = i + 1 < n ? i + 1 : (closed ? 1 : n - 1);
                float tx = xs[next] - xs[prev];
                float ty = ys[next] - ys[prev];
                const float len = std::hypot(tx, ty);
                tx = len > 1e-6f ? tx / len : 1.0f;
                ty = len > 1e-6f ? ty / len : 0.0f;
                if (i > 0) {
                    along += std::hypot(xs[i] - xs[i - 1], ys[i] - ys[i - 1]);
                }
                for (const float side : { -1.0f, 1.0f }) {
                    vertices.push_back({ { xs[i], ys[i] }, { -ty, tx }, along, side, c, provinces });
                }
                if (i + 1 < n) {
                    const auto k = static_cast<std::uint32_t>(base + i * 2);
                    indices.insert(indices.end(), { k, k + 1, k + 2, k + 1, k + 3, k + 2 });
                }
            }
        }
        border_index_count_ = static_cast<UINT>(indices.size());
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = static_cast<UINT>(std::max<std::size_t>(indices.size(), 1) * sizeof(std::uint32_t));
        ib.Usage = D3D11_USAGE_IMMUTABLE;
        ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        indices.resize(std::max<std::size_t>(indices.size(), 1));
        const D3D11_SUBRESOURCE_DATA iinit{ indices.data(), 0, 0 };
        std::vector<BorderStyle> none(std::max<std::uint32_t>(chain_count_, 1), BorderStyle::none);
        return check(device_->CreateBuffer(&ib, &iinit, &border_indices_), "border indices", error) &&
               structured(device_.Get(), vertices.size(), vertices.data(), false, nullptr, &border_vertices_,
                          "border vertices", error) &&
               structured(device_.Get(), none.size(), none.data(), true, &border_styles_buffer_, &border_styles_,
                          "border styles", error);
    }

    bool MapRenderer::create_states(std::string* error)
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;

        D3D11_BLEND_DESC opaque{};
        opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_BLEND_DESC alpha = opaque;
        auto& rt = alpha.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                   D3D11_COLOR_WRITE_ENABLE_BLUE;

        D3D11_DEPTH_STENCIL_DESC write{};
        write.DepthEnable = TRUE;
        write.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        write.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        D3D11_DEPTH_STENCIL_DESC read = write;
        read.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        // Borders: each pixel is blended once per rank (see vs_border), so strips overlapping at junctions do not darken.
        D3D11_DEPTH_STENCIL_DESC border = read;
        border.StencilEnable = TRUE;
        border.StencilReadMask = border.StencilWriteMask = 0xFF;
        border.FrontFace.StencilFunc = D3D11_COMPARISON_GREATER;   // ref > stored: only over lower ranks
        border.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        border.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        border.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        border.BackFace = border.FrontFace;

        for (UINT pass = 0; pass < 2; ++pass) {
            const std::uint32_t data[4] = { pass, 0, 0, 0 };
            D3D11_BUFFER_DESC pd{};
            pd.ByteWidth = sizeof(data);
            pd.Usage = D3D11_USAGE_IMMUTABLE;
            pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
            if (!check(device_->CreateBuffer(&pd, &init, &border_pass_[pass]), "border pass", error)) {
                return false;
            }
        }
        return check(device_->CreateSamplerState(&sd, &linear_), "sampler", error) &&
               check(device_->CreateRasterizerState(&rs, &rasterizer_), "rasterizer", error) &&
               check(device_->CreateBlendState(&opaque, &opaque_), "blend", error) &&
               check(device_->CreateBlendState(&alpha, &alpha_), "blend", error) &&
               check(device_->CreateDepthStencilState(&write, &depth_write_), "depth", error) &&
               check(device_->CreateDepthStencilState(&border, &depth_border_), "depth", error);
    }

    bool MapRenderer::ensure_depth(ID3D11RenderTargetView* target, UINT* width, UINT* height)
    {
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        *width = desc.Width;
        *height = desc.Height;
        if (depth_ && depth_width_ == desc.Width && depth_height_ == desc.Height) {
            return true;
        }
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = desc.Width;
        dd.Height = desc.Height;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        depth_.Reset();
        depth_texture_.Reset();
        if (FAILED(device_->CreateTexture2D(&dd, nullptr, &depth_texture_)) ||
            FAILED(device_->CreateDepthStencilView(depth_texture_.Get(), nullptr, &depth_))) {
            return false;
        }
        depth_width_ = desc.Width;
        depth_height_ = desc.Height;
        return true;
    }

    void MapRenderer::set_labels(const MapData& map, const LabelFont& font, std::span<const MapLabel> labels,
                                 std::span<const std::uint32_t> keys)
    {
        labels_.set_labels(device_.Get(), map, font, labels, keys);
    }

    void MapRenderer::set_province_labels(const MapData& map, const LabelFont& font, std::span<const MapLabel> labels,
                                          std::span<const std::uint32_t> keys)
    {
        province_labels_.set_labels(device_.Get(), map, font, labels, keys);
    }

    void MapRenderer::set_colors(ID3D11DeviceContext* context, std::span<const ProvinceFill> fills,
                                 std::span<const BorderStyle> styles)
    {
        if (fills.size() == province_count_) {
            upload(context, fills_buffer_.Get(), fills);
        }
        if (styles.size() == chain_count_ && chain_count_ > 0) {
            upload(context, border_styles_buffer_.Get(), styles);
        }
    }

    void MapRenderer::set_selection(ID3D11DeviceContext* context, std::span<const std::uint8_t> selected)
    {
        std::vector<std::uint32_t> flags(province_count_, 0);
        for (std::size_t i = 0; i < std::min(selected.size(), flags.size()); ++i) {
            flags[i] = selected[i];
        }
        upload(context, selection_buffer_.Get(), std::span<const std::uint32_t>(flags));
    }

    void MapRenderer::draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, const FrameView& view)
    {
        UINT width = 0;
        UINT height = 0;
        if (!ensure_depth(target, &width, &height)) {
            return;
        }
        const CameraView& camera = view.camera;

        FrameConstants constants{};
        std::memcpy(constants.view_proj, camera.view_proj.m, sizeof(constants.view_proj));
        constants.eye[0] = camera.eye.x;
        constants.eye[1] = camera.eye.y;
        constants.eye[2] = camera.eye.z;
        constants.time = view.time;
        constants.map_size[0] = map_width_;
        constants.map_size[1] = map_height_;
        constants.pixel_angle = camera.pixel_angle;
        constants.map_mode = view.map_mode;
        constants.hovered = view.hovered;
        upload(context, frame_constants_.Get(), std::span<const FrameConstants>(&constants, 1));

        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &target, depth_.Get());
        context->ClearRenderTargetView(target, kClear);
        context->ClearDepthStencilView(depth_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer_.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cbs[] = { frame_constants_.Get() };
        context->VSSetConstantBuffers(0, 1, cbs);
        context->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* samplers[] = { linear_.Get() };
        context->VSSetSamplers(0, 1, samplers);
        context->PSSetSamplers(0, 1, samplers);
        ID3D11ShaderResourceView* common[] = { province_ids_.Get(), fills_.Get(), nullptr, selection_.Get() };
        context->VSSetShaderResources(0, 4, common);
        context->PSSetShaderResources(0, 4, common);

        // The blocks (they also leave their data bound for the borders' vertex shader).
        context->OMSetBlendState(opaque_.Get(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth_write_.Get(), 0);
        terrain_.draw(context);

        // Borders, blended over the blocks.
        context->OMSetBlendState(alpha_.Get(), nullptr, 0xFFFFFFFF);
        if (border_index_count_ > 0) {
            ID3D11ShaderResourceView* border_srv[] = { border_styles_.Get(), border_vertices_.Get() };
            context->VSSetShaderResources(5, 2, border_srv);
            context->IASetIndexBuffer(border_indices_.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->VSSetShader(border_vs_.Get(), nullptr, 0);
            context->PSSetShader(border_ps_.Get(), nullptr, 0);
            for (UINT pass = 0; pass < 2; ++pass) {   // countries (stencil 2), then the rest (stencil 1)
                ID3D11Buffer* pass_cb[] = { border_pass_[pass].Get() };
                context->VSSetConstantBuffers(1, 1, pass_cb);
                context->OMSetDepthStencilState(depth_border_.Get(), 2 - pass);
                context->DrawIndexed(border_index_count_, 0, 0);
            }
        }

        // Map names over everything on the map.
        labels_.draw(context, view.mode_names_alpha);
        province_labels_.draw(context, view.province_names_alpha);

        ID3D11ShaderResourceView* nulls[19] = {};
        context->VSSetShaderResources(0, 19, nulls);
        context->PSSetShaderResources(0, 19, nulls);
        ID3D11RenderTargetView* no_target = nullptr;
        context->OMSetRenderTargets(1, &no_target, nullptr);
    }
}
