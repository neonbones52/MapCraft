#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::minecraft
{
    class Assets;

    // A block with its properties, as structure palettes and feature configs give it. The name has no namespace
    // ("oak_stairs"); properties are sorted by name.
    struct BlockState
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> properties;

        [[nodiscard]] std::string_view property(std::string_view key) const;
        [[nodiscard]] std::string key() const;   // "oak_stairs[facing=east,half=bottom]"
        bool operator==(const BlockState&) const = default;
    };
    // "minecraft:oak_stairs[facing=east,half=bottom]" or "oak_planks".
    [[nodiscard]] BlockState parse_block_state(std::string_view text);
    void sort_properties(BlockState* state);

    enum class Direction : std::uint8_t
    {
        down,
        up,
        north,   // -z
        south,   // +z
        west,    // -x
        east,    // +x
        none,
    };

    // One face of a baked block model, in block units (0..1 inside the block; x east, y up, z south). Corners are
    // counter-clockwise seen from the face's front.
    struct ModelQuad
    {
        std::array<std::array<float, 3>, 4> pos{};
        std::array<std::array<float, 2>, 4> uv{};   // 0..1 across the texture
        std::string texture;                        // "block/oak_planks"
        Direction cull = Direction::none;           // hidden when the neighbour on that side covers it
        bool tinted = false;                        // takes the block's colour (BlockColors), see block_tint
        float shade = 1.0f;                         // Minecraft's directional shade of its normal (1 without shade)
    };

    struct BakedBlock
    {
        std::vector<ModelQuad> quads;
        bool cube = false;   // one full 16^3 element with all six faces: it may hide its neighbours' faces
    };

    // Minecraft's block colours for tinted faces: grass, foliage (both from the biome), or a fixed colour.
    struct BlockTint
    {
        enum class Kind : std::uint8_t
        {
            none,
            grass,
            foliage,
            fixed,
        };
        Kind kind = Kind::none;
        std::uint32_t rgb = 0xFFFFFF;
    };
    [[nodiscard]] BlockTint block_tint(std::string_view block);

    // Block models from the jar (assets/minecraft/blockstates, models/block): variants and multipart, parent chains,
    // texture variables, element and face rotations, the variants' x / y rotations. Blocks drawn as entities (beds,
    // chests, signs) have no elements and bake to nothing. Baked states are cached.
    class BlockModels
    {
    public:
        explicit BlockModels(const Assets& assets);
        ~BlockModels();
        BlockModels(const BlockModels&) = delete;
        BlockModels& operator=(const BlockModels&) = delete;

        // The state's faces; null with `error` set when its block state or a model cannot be read.
        const BakedBlock* bake(const BlockState& state, std::string* error);

    private:
        struct Model;
        const Model* model(const std::string& name, std::string* error, int depth = 0);

        const Assets& assets_;
        std::unordered_map<std::string, std::unique_ptr<Model>> models_;
        std::unordered_map<std::string, std::unique_ptr<BakedBlock>> baked_;
    };
}
