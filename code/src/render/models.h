#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "view/camera.h"

namespace mc
{
    // The models standing on the map (houses, piers, trees), prepared on the CPU (app/scenery.cpp): their textures,
    // their meshes and every placed copy. The renderer knows no files, blocks or biomes.
    struct ModelSet
    {
        static constexpr std::uint32_t kTextureSize = 16;   // px per layer side

        enum class Tint : std::uint32_t
        {
            none = 0,
            grass = 1,     // the instance's grass colour
            foliage = 2,   // the instance's foliage colour
            fixed = 3,     // the vertex's own colour
        };
        // A triangle-list vertex in model space: Minecraft blocks, x east, y up, z south, the model's ground point
        // at the origin.
        struct Vertex
        {
            float pos[3];
            float uv[2];
            std::uint32_t layer_shade_tint;   // texture layer | shade (0..255) << 16 | Tint << 24
            std::uint32_t tint;               // RGBA8 for Tint::fixed
        };
        static_assert(sizeof(Vertex) == 28);
        // What a mesh belongs to: each group shows and fades out by itself (app.cpp, the zoom levels).
        enum class Group : std::uint32_t
        {
            settlements = 0,   // houses and piers
            trees = 1,
        };
        static constexpr std::size_t kGroups = 2;
        struct Mesh
        {
            std::uint32_t first = 0;   // vertex
            std::uint32_t count = 0;
            Group group = Group::settlements;
        };
        // A placed copy: world position of its ground point, world units per block, a quarter turn (0..3, clockwise
        // seen from above, as Minecraft turns north to east) with flags, and the colours of the ground it stands on.
        static constexpr std::uint32_t kTurnMask = 3;
        static constexpr std::uint32_t kFilled = 4;   // takes the map mode's fill of the province it stands in
        struct Instance
        {
            float pos[3];
            float scale;
            std::uint32_t mesh;
            std::uint32_t turn_flags;   // turn | kFilled
            std::uint32_t foliage;   // RGBA8
            std::uint32_t grass;     // RGBA8
        };
        static_assert(sizeof(Instance) == 32);

        std::vector<std::uint8_t> layers;   // layer_count * kTextureSize^2 RGBA8
        std::uint32_t layer_count = 0;
        std::vector<Vertex> vertices;
        std::vector<Mesh> meshes;
        std::vector<Instance> instances;
    };

    // Draws the model set (docs/RENDER.md, "Модели на карте"): instances sorted into chunks of the map; each frame
    // only the chunks in view are drawn, mesh by mesh. Texels with alpha under one half are cut out; a group fading
    // in or out dissolves in an ordered dither (no blending, so no sorting).
    class ModelLayer
    {
    public:
        bool init(ID3D11Device* device, const ModelSet& set, float map_width, float map_height, std::string* error);
        // Draws the instances in the camera's view (the map renderer has set the frame constants and depth); `fades`
        // per group: 0 hidden, 1 whole.
        void draw(ID3D11DeviceContext* context, const CameraView& camera,
                  const std::array<float, ModelSet::kGroups>& fades);
        [[nodiscard]] std::size_t instance_count() const { return instances_.size(); }
        [[nodiscard]] std::size_t vertex_count() const { return vertex_count_; }

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        struct Chunk
        {
            float lo[3];
            float hi[3];
            std::uint32_t first = 0;   // into instances_
            std::uint32_t count = 0;
        };

        std::vector<ModelSet::Mesh> meshes_;
        std::vector<ModelSet::Instance> instances_;   // sorted by chunk, then mesh
        std::vector<Chunk> chunks_;
        std::vector<std::uint32_t> visible_;          // chunk indices drawn last
        std::vector<std::vector<ModelSet::Instance>> by_mesh_;
        std::vector<std::uint32_t> draw_first_;       // per mesh: its first instance in the upload
        std::vector<std::uint32_t> draw_count_;
        std::size_t vertex_count_ = 0;

        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11ShaderResourceView> vertices_;
        ComPtr<ID3D11Buffer> instance_buffer_;
        ComPtr<ID3D11ShaderResourceView> instance_view_;
        ComPtr<ID3D11ShaderResourceView> layers_;
        ComPtr<ID3D11Buffer> draw_constants_;
        ComPtr<ID3D11SamplerState> point_;
    };
}
