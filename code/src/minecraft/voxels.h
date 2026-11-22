#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "minecraft/blocks.h"

namespace mc::minecraft
{
    class Assets;

    // Blocks in a small grid (x east, y up, z south), as a structure file or a grown feature gives them.
    struct VoxelModel
    {
        struct Voxel
        {
            int x = 0;
            int y = 0;
            int z = 0;
            std::uint32_t state = 0;   // index into states
        };
        std::array<int, 3> size{};     // structure size; features: their bounding box
        std::vector<BlockState> states;
        std::vector<Voxel> voxels;

        // Places a block (replacing what is there).
        void set(int x, int y, int z, const BlockState& state);
        [[nodiscard]] bool occupied(int x, int y, int z) const;
    };

    // data/minecraft/structure/<name>.nbt: its first palette; jigsaw blocks become their final state, air and
    // structure voids are left out.
    bool load_structure(const Assets& assets, std::string_view name, VoxelModel* out, std::string* error);

    // data/minecraft/worldgen/configured_feature/<name>.json grown once from `seed`, its root block at (0, 0, 0):
    // trees (straight and forking trunks; blob, spruce and acacia foliage, as Minecraft's placers; decorators such
    // as vines and cocoa are left out), block columns (cactus) and single blocks (dead bush).
    bool grow_feature(const Assets& assets, std::string_view name, std::uint32_t seed, VoxelModel* out,
                      std::string* error);

    // A triangle list of baked block faces, in blocks: x east, y up, z south.
    struct MeshVertex
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        float shade = 1.0f;
        std::uint32_t texture = 0;     // the TextureSource's index
        BlockTint tint;                // the block's colour where the face is tinted
    };

    // Where meshes get their textures: an index per texture ("block/oak_planks"), and whether it has no
    // transparent texel (so a full block of it hides the faces behind it).
    class TextureSource
    {
    public:
        virtual ~TextureSource() = default;
        virtual bool index(const std::string& texture, std::uint32_t* out, std::string* error) = 0;
        [[nodiscard]] virtual bool opaque(std::uint32_t index) const = 0;
    };

    // The model's faces, less those a neighbour hides: a face with a cull side is left out next to an opaque full
    // block, or next to a full block of the same kind (leaves against leaves, glass against glass), and a face
    // culled downward at y = 0 (the model stands on the ground there). `origin` is subtracted from every position
    // (the point that stands on the ground).
    bool build_mesh(BlockModels& models, TextureSource& textures, const VoxelModel& model,
                    const std::array<float, 3>& origin, std::vector<MeshVertex>* out, std::string* error);
}
