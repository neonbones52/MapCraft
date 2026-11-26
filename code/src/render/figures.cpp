#include "render/figures.h"

#include <algorithm>
#include <cstring>

#include "render/d3d_util.h"
#include "render/shaders.h"

namespace mc
{
    namespace
    {
        // The draw's model and figures: SV_VertexID and SV_InstanceID start at 0 in every draw.
        struct DrawConstants
        {
            std::uint32_t first_instance;
            std::uint32_t first_vertex;
            float fade;
            std::uint32_t pad;
        };
    }

    bool FigureLayer::init(ID3D11Device* device, const FigureSet& set, std::string* error)
    {
        device_ = device;
        ComPtr<ID3D10Blob> vs;
        ComPtr<ID3D10Blob> ps;
        if (!shaders::compile(shaders::map_source, "map.hlsl", "vs_figure", "vs_5_0", &vs, error) ||
            !shaders::compile(shaders::map_source, "map.hlsl", "ps_figure", "ps_5_0", &ps, error) ||
            !d3d::check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_),
                        "vs_figure", error) ||
            !d3d::check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_),
                        "ps_figure", error) ||
            !d3d::point_sampler(device, &point_, error)) {
            return false;
        }
        models_ = set.models;
        model_first_.assign(models_.size(), 0);
        model_count_.assign(models_.size(), 0);
        std::vector<std::uint8_t> layers = set.layers;
        std::uint32_t layer_count = set.layer_count;
        if (layer_count == 0) {   // an empty set still binds valid resources
            layers.assign(std::size_t{ FigureSet::kTextureSize } * FigureSet::kTextureSize * 4, 255);
            layer_count = 1;
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(DrawConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return d3d::structured(device, set.vertices, &vertices_, "figure vertices", error) &&
               d3d::texture_array(device, layers, layer_count, FigureSet::kTextureSize, &layers_, "figure textures",
                                  error) &&
               d3d::check(device->CreateBuffer(&cb, nullptr, &draw_constants_), "figure draw constants", error) &&
               reserve(64, 64 * 8);
    }

    bool FigureLayer::reserve(std::size_t figures, std::size_t matrices)
    {
        std::string error;
        if (figures > instance_capacity_) {
            instance_capacity_ = std::max(figures, instance_capacity_ * 2);
            if (!d3d::dynamic_structured(device_.Get(), instance_capacity_, sizeof(std::uint32_t), &instance_buffer_,
                                         &instance_view_, "figure instances", &error)) {
                instance_capacity_ = 0;
                return false;
            }
        }
        if (matrices > matrix_capacity_) {
            matrix_capacity_ = std::max(matrices, matrix_capacity_ * 2);
            if (!d3d::dynamic_structured(device_.Get(), matrix_capacity_, sizeof(FigureMatrix), &matrix_buffer_,
                                         &matrix_view_, "figure matrices", &error)) {
                matrix_capacity_ = 0;
                return false;
            }
        }
        return true;
    }

    void FigureLayer::draw(ID3D11DeviceContext* context, std::span<const FigureDraw> figures,
                           std::span<const FigureMatrix> matrices, float fade)
    {
        if (fade <= 0.0f || figures.empty() || matrices.empty() || !reserve(figures.size(), matrices.size())) {
            return;
        }
        // The figures grouped by model, each a first matrix.
        std::ranges::fill(model_count_, 0u);
        for (const FigureDraw& f : figures) {
            if (f.model < models_.size()) {
                ++model_count_[f.model];
            }
        }
        std::uint32_t at = 0;
        for (std::size_t m = 0; m < models_.size(); ++m) {
            model_first_[m] = at;
            at += model_count_[m];
        }
        by_model_.assign(at, 0);
        std::vector<std::uint32_t> next = model_first_;
        for (const FigureDraw& f : figures) {
            if (f.model < models_.size()) {
                by_model_[next[f.model]++] = f.first_matrix;
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (by_model_.empty() || FAILED(context->Map(instance_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        std::memcpy(mapped.pData, by_model_.data(), by_model_.size() * sizeof(std::uint32_t));
        context->Unmap(instance_buffer_.Get(), 0);
        if (FAILED(context->Map(matrix_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        std::memcpy(mapped.pData, matrices.data(), matrices.size_bytes());
        context->Unmap(matrix_buffer_.Get(), 0);

        ID3D11ShaderResourceView* vs_srv[] = { vertices_.Get(), instance_view_.Get(), matrix_view_.Get() };
        context->VSSetShaderResources(22, 3, vs_srv);
        ID3D11ShaderResourceView* ps_srv[] = { layers_.Get() };
        context->PSSetShaderResources(25, 1, ps_srv);
        ID3D11SamplerState* samplers[] = { point_.Get() };
        context->PSSetSamplers(1, 1, samplers);
        ID3D11Buffer* cbs[] = { draw_constants_.Get() };
        context->VSSetConstantBuffers(4, 1, cbs);
        context->PSSetConstantBuffers(4, 1, cbs);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        for (std::size_t m = 0; m < models_.size(); ++m) {
            if (model_count_[m] == 0 || models_[m].count == 0) {
                continue;
            }
            if (FAILED(context->Map(draw_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            const DrawConstants constants{ model_first_[m], models_[m].first, std::min(fade, 1.0f), 0 };
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(draw_constants_.Get(), 0);
            context->DrawInstanced(models_[m].count, model_count_[m], 0, 0);
        }
    }
}
