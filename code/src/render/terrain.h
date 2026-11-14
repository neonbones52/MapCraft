#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc
{
    class MapData;

    // What the blocks look like, prepared on the CPU (app/terrain_look.cpp): the block textures, the materials that
    // pick them by face and depth, and per block its material and tints. The renderer knows no files or biomes.
    struct TerrainLook
    {
        static constexpr std::uint32_t kTextureSize = 16;   // px per layer side
        static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

        enum class Tint : std::uint32_t
        {
            none = 0,
            grass = 1,
            water = 2,
        };
        // A texture of the array: `frames` consecutive layers from `first_layer`, `frame_time` s each.
        struct Texture
        {
            std::uint32_t first_layer = 0;
            std::uint32_t frames = 1;
            float frame_time = 0.05f;
            Tint tint = Tint::none;
        };
        // Texture indices of a block's faces. Sides go by depth under the column's top: the top block shows `side`
        // (with `overlay` over it, kNone = none), the next `kUnderDepth` blocks `under`, deeper ones `deep`.
        struct Material
        {
            std::uint32_t top = 0;
            std::uint32_t side = 0;
            std::uint32_t overlay = kNone;
            std::uint32_t under = 0;
            std::uint32_t deep = 0;
            std::uint32_t pad[3] = {};
        };
        static constexpr std::uint32_t kUnderDepth = 3;

        std::vector<std::uint8_t> layers;   // layer_count * kTextureSize^2 RGBA8, rows top down
        std::uint32_t layer_count = 0;
        std::vector<Texture> textures;
        std::vector<Material> materials;
        std::uint32_t water_still = 0;      // texture of water surfaces
        std::uint32_t water_flow = 0;       // texture of water falling down a step
        // Per block (MapData block grid, rows top down).
        std::vector<std::uint8_t> block_material;
        std::vector<std::uint32_t> grass_tint;   // RGBA8, blended between biomes
        std::vector<std::uint32_t> water_tint;
    };

    // The voxel surface (docs/RENDER.md): one height per block (MapData BHGT, BWTR), drawn as its visible faces only,
    // the tops and the sides of steps, merged into runs of equal height. What a face shows (texture, tint, Minecraft
    // face shading and ambient occlusion, map mode fill) is worked out per pixel from the block textures, so a run
    // can cross blocks of any material. Water is opaque: nothing under it is drawn. Where a river or lake stands
    // above water next to it, the lower block is a waterfall: its water slopes up to the higher one (Minecraft's
    // flowing water), its sides reach up under the slope.
    class TerrainLayer
    {
    public:
        bool init(ID3D11Device* device, const MapData& map, const TerrainLook& look, std::string* error);
        // Binds the terrain's resources (t4, t7..t12) for the map shaders and draws the faces.
        void draw(ID3D11DeviceContext* context);
        // The block data (t4), also read by the border strips to lie on the surface.
        [[nodiscard]] ID3D11ShaderResourceView* blocks() const { return blocks_.Get(); }
        [[nodiscard]] std::uint32_t quad_count() const { return quad_count_; }

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        // Per block: the Face toward the higher water falling into it, 0 = none.
        static std::vector<std::uint8_t> waterfalls(const MapData& map);
        bool create_quads(ID3D11Device* device, const MapData& map, const std::vector<std::uint8_t>& falls,
                          std::string* error);
        bool create_textures(ID3D11Device* device, const MapData& map, const TerrainLook& look,
                             const std::vector<std::uint8_t>& falls, std::string* error);

        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11ShaderResourceView> quads_;
        ComPtr<ID3D11ShaderResourceView> blocks_;        // RGBA8 uint: ground, water, material, class | fall << 4
        ComPtr<ID3D11ShaderResourceView> array_;         // block textures with mips
        ComPtr<ID3D11ShaderResourceView> textures_;
        ComPtr<ID3D11ShaderResourceView> materials_;
        ComPtr<ID3D11ShaderResourceView> grass_tint_;
        ComPtr<ID3D11ShaderResourceView> water_tint_;
        ComPtr<ID3D11Buffer> constants_;                 // b2
        ComPtr<ID3D11SamplerState> point_;               // s1
        std::uint32_t quad_count_ = 0;
    };
}
