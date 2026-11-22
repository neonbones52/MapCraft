#include "render/models.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/d3d_util.h"
#include "render/shaders.h"

namespace mc
{
    namespace
    {
        constexpr float kChunk = 128.0f;          // map px per chunk side (32 blocks)
        constexpr float kReach = 16.0f;           // blocks a model may reach from its ground point, sideways
        constexpr float kHeight = 24.0f;          // and up

        // The draw's mesh and instances: SV_VertexID and SV_InstanceID start at 0 in every draw.
        struct DrawConstants
        {
            std::uint32_t first_instance;
            std::uint32_t first_vertex;
            float fade;   // the mesh's group: 0 hidden, 1 whole
            std::uint32_t pad;
        };
    }

    bool ModelLayer::init(ID3D11Device* device, const ModelSet& set, float map_width, float map_height,
                          std::string* error)
    {
        ComPtr<ID3D10Blob> vs;
        ComPtr<ID3D10Blob> ps;
        if (!shaders::compile(shaders::map_source, "map.hlsl", "vs_model", "vs_5_0", &vs, error) ||
            !shaders::compile(shaders::map_source, "map.hlsl", "ps_model", "ps_5_0", &ps, error) ||
            !d3d::check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_),
                        "vs_model", error) ||
            !d3d::check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_),
                        "ps_model", error) ||
            !d3d::point_sampler(device, &point_, error)) {
            return false;
        }
        meshes_ = set.meshes;
        vertex_count_ = set.vertices.size();

        // Instances into chunks of the map, each chunk's sorted by mesh.
        const auto columns = static_cast<std::uint32_t>(std::ceil(map_width / kChunk));
        const auto rows = static_cast<std::uint32_t>(std::ceil(map_height / kChunk));
        auto chunk_of = [&](const ModelSet::Instance& i) {
            const auto cx = static_cast<std::uint32_t>(std::clamp(i.pos[0] / kChunk, 0.0f, columns - 1.0f));
            const auto cy = static_cast<std::uint32_t>(std::clamp(-i.pos[2] / kChunk, 0.0f, rows - 1.0f));
            return cy * columns + cx;
        };
        instances_ = set.instances;
        std::ranges::stable_sort(instances_, [&](const ModelSet::Instance& a, const ModelSet::Instance& b) {
            const std::uint32_t ca = chunk_of(a);
            const std::uint32_t cb = chunk_of(b);
            return ca != cb ? ca < cb : a.mesh < b.mesh;
        });
        for (std::uint32_t i = 0; i < instances_.size(); ++i) {
            const ModelSet::Instance& inst = instances_[i];
            const std::uint32_t c = chunk_of(inst);
            if (chunks_.empty() || chunk_of(instances_[chunks_.back().first]) != c) {
                Chunk chunk;
                chunk.first = i;
                std::fill(std::begin(chunk.lo), std::end(chunk.lo), 1e30f);
                std::fill(std::begin(chunk.hi), std::end(chunk.hi), -1e30f);
                chunks_.push_back(chunk);
            }
            Chunk& chunk = chunks_.back();
            ++chunk.count;
            const float reach = kReach * inst.scale;
            const float lo[3] = { inst.pos[0] - reach, inst.pos[1] - reach, inst.pos[2] - reach };
            const float hi[3] = { inst.pos[0] + reach, inst.pos[1] + kHeight * inst.scale, inst.pos[2] + reach };
            for (int k = 0; k < 3; ++k) {
                chunk.lo[k] = std::min(chunk.lo[k], lo[k]);
                chunk.hi[k] = std::max(chunk.hi[k], hi[k]);
            }
        }
        by_mesh_.resize(meshes_.size());
        draw_first_.assign(meshes_.size(), 0);
        draw_count_.assign(meshes_.size(), 0);

        // An empty set still binds valid resources.
        std::vector<std::uint8_t> layers = set.layers;
        std::uint32_t layer_count = set.layer_count;
        if (layer_count == 0) {
            layers.assign(std::size_t{ ModelSet::kTextureSize } * ModelSet::kTextureSize * 4, 255);
            layer_count = 1;
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(DrawConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return d3d::structured(device, set.vertices, &vertices_, "model vertices", error) &&
               d3d::dynamic_structured(device, instances_.size(), sizeof(ModelSet::Instance), &instance_buffer_,
                                       &instance_view_, "model instances", error) &&
               d3d::texture_array(device, layers, layer_count, ModelSet::kTextureSize, &layers_, "model textures",
                                  error) &&
               d3d::check(device->CreateBuffer(&cb, nullptr, &draw_constants_), "model draw constants", error);
    }

    void ModelLayer::draw(ID3D11DeviceContext* context, const CameraView& camera,
                          const std::array<float, ModelSet::kGroups>& fades)
    {
        if (instances_.empty() || std::ranges::all_of(fades, [](float f) { return f <= 0.0f; })) {
            return;
        }
        float planes[6][4];
        camera.frustum(planes);
        std::vector<std::uint32_t> visible;
        for (std::uint32_t c = 0; c < chunks_.size(); ++c) {
            if (box_visible(planes, chunks_[c].lo, chunks_[c].hi)) {
                visible.push_back(c);
            }
        }
        if (visible.empty()) {
            visible_.clear();
            return;
        }
        if (visible != visible_) {
            // The chunks in view, gathered mesh by mesh into the instance buffer.
            for (auto& list : by_mesh_) {
                list.clear();
            }
            for (const std::uint32_t c : visible) {
                for (std::uint32_t i = chunks_[c].first; i < chunks_[c].first + chunks_[c].count; ++i) {
                    by_mesh_[instances_[i].mesh].push_back(instances_[i]);
                }
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(instance_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            auto* out = static_cast<ModelSet::Instance*>(mapped.pData);
            std::uint32_t at = 0;
            for (std::size_t m = 0; m < by_mesh_.size(); ++m) {
                draw_first_[m] = at;
                draw_count_[m] = static_cast<std::uint32_t>(by_mesh_[m].size());
                if (!by_mesh_[m].empty()) {
                    std::memcpy(out + at, by_mesh_[m].data(), by_mesh_[m].size() * sizeof(ModelSet::Instance));
                }
                at += draw_count_[m];
            }
            context->Unmap(instance_buffer_.Get(), 0);
            visible_ = std::move(visible);
        }

        ID3D11ShaderResourceView* vs_srv[] = { vertices_.Get(), instance_view_.Get() };
        context->VSSetShaderResources(19, 2, vs_srv);
        ID3D11ShaderResourceView* ps_srv[] = { layers_.Get() };
        context->PSSetShaderResources(21, 1, ps_srv);
        ID3D11SamplerState* samplers[] = { point_.Get() };
        context->PSSetSamplers(1, 1, samplers);
        ID3D11Buffer* cbs[] = { draw_constants_.Get() };
        context->VSSetConstantBuffers(3, 1, cbs);
        context->PSSetConstantBuffers(3, 1, cbs);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        for (std::size_t m = 0; m < meshes_.size(); ++m) {
            const float fade = fades[static_cast<std::size_t>(meshes_[m].group)];
            if (draw_count_[m] == 0 || meshes_[m].count == 0 || fade <= 0.0f) {
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(draw_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            const DrawConstants constants{ draw_first_[m], meshes_[m].first, std::min(fade, 1.0f), 0 };
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(draw_constants_.Get(), 0);
            context->DrawInstanced(meshes_[m].count, draw_count_[m], 0, 0);
        }
    }
}
