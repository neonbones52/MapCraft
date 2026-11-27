#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "minecraft/assets.h"

// Entity models as Minecraft 26.2 draws them (its model code read with javap, as in the CinemaShort studio's
// tools/mc_character.py and tools/mc_horse.py): boxes on parts, box UVs into the entity's texture, a pose per part.
// Built from the player's jar while the game runs; nothing is written anywhere.
//
// Model space is the game's: x = the entity's left, y = down, z = back, one unit = one model pixel (1/16 block), the
// ground at y = 24. A vertex lies in its part's space: relative to the part's pivot, before the part turns.
namespace mc::minecraft
{
    struct EntityVertex
    {
        float pos[3];
        float normal[3];
        float uv[2];              // 0..1 over a texture layer
        std::uint16_t layer = 0;  // into EntityTextures
        std::uint16_t part = 0;   // into EntityModel::parts
    };

    struct EntityModel
    {
        std::vector<EntityVertex> vertices;
        std::vector<std::array<float, 3>> pivots;   // per part, model px
    };

    // The textures of all models, one per layer, each at the top left of a kSize x kSize layer (a 64 x 32 armour
    // texture fills the top half, the 16 x 16 sword a corner).
    struct EntityTextures
    {
        static constexpr std::uint32_t kSize = 64;
        std::vector<std::uint8_t> layers;   // count * kSize^2 RGBA8
        std::uint32_t count = 0;

        // Adds an image (at most kSize on a side) as a new layer; its index.
        std::uint16_t add(const Image& image);
    };

    // A part's turn, radians, about its pivot, as the game's PartPose: R = Rz * Ry * Rx.
    struct PartRotation
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // The villager in a full set of iron armour with an iron sword in the right hand (the soldiers of the film "All
    // Quiet on the Overworld"): the villager's head and body, the zombie villager's separate arms (so the skin gets a
    // hand under the sleeve), the plains type with the weaponsmith's layer.
    enum VillagerPart : std::uint16_t
    {
        villager_head,
        villager_body,
        villager_arm_right,
        villager_arm_left,
        villager_leg_right,
        villager_leg_left,
        villager_part_count,
    };
    bool iron_soldier(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error);

    // The brown horse in iron horse armour with a saddle, drawn at the game's 1.1 (kHorseScale, not in the mesh).
    enum HorsePart : std::uint16_t
    {
        horse_body,
        horse_neck,
        horse_leg_left_hind,
        horse_leg_right_hind,
        horse_leg_left_front,
        horse_leg_right_front,
        horse_tail,
        horse_part_count,
    };
    inline constexpr float kHorseScale = 1.1f;
    // Where a rider's ground point sits on the horse, horse model px (the saddle; CinemaShort's mount, by eye).
    inline constexpr float kHorseSeat[3] = { 0.0f, 14.0f, 1.0f };
    bool iron_horse(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error);

    // The field gun of the artillery (not a game entity: the user's design, built as the game's block models are,
    // each face its own block texture): a cast-iron barrel (polished blackstone) on a spruce carriage, two spruce
    // wheels a block across, their faces drawn from spruce textures here (rim, 8 spokes, hub). The muzzle to the
    // front (-z), the axle at y 16; 14 boxes.
    enum GunPart : std::uint16_t
    {
        gun_carriage,
        gun_barrel,
        gun_wheel_left,
        gun_wheel_right,
        gun_part_count,
    };
    bool field_gun(const Assets& assets, EntityTextures* textures, EntityModel* out, std::string* error);

    // Poses, the game's setupAnim: `age` in ticks (20 a second); the walk as the game's WalkAnimationState, `walk`
    // its position (it grows by `amount` a tick) and `amount` its speed (0 standing, 1 at full speed).
    // The soldier holds the sword up (ArmPose.ITEM) and its arms bob as a humanoid's do (AnimationUtils.bobArms);
    // walking, its legs and arms swing as HumanoidModel's (the sword arm's swing halved by the item pose); `riding`:
    // the rider's legs and arms (HumanoidModel while riding), which do not walk. `attack`: the game's attackTime, how
    // far into a swing of the sword (0 none, up to 1; HumanoidModel.setupAttackAnimation, the WHACK swing of
    // kSwingTicks): the body turns, the arms go round with it, the sword arm strikes down.
    inline constexpr float kSwingTicks = 6.0f;   // SwingAnimation.DEFAULT
    struct SoldierPose
    {
        std::array<PartRotation, villager_part_count> turn{};
        std::array<std::array<float, 3>, villager_part_count> shift{};   // of each part's pivot from the model's, px
    };
    void soldier_pose(float age, bool riding, float walk, float amount, float attack, SoldierPose* out);
    // The horse: its neck and tail at their rest angles; walking, its legs swing as AbstractEquineModel's, the head
    // nods and the tail lifts.
    void horse_pose(float age, float walk, float amount, std::array<PartRotation, horse_part_count>* out);
    // The gun: rolling, its wheels turn with the walk's position (`walk`, as the soldier beside it walks); `shot`:
    // ticks into a shot (< 0 none, up to kShotTicks): the gun jumps back and its barrel kicks up, then it rolls
    // forward to where it stood, the wheels turning with it.
    inline constexpr float kShotTicks = 30.0f;
    struct GunPose
    {
        std::array<PartRotation, gun_part_count> turn{};
        std::array<std::array<float, 3>, gun_part_count> shift{};   // of each part's pivot from the model's, px
    };
    void gun_pose(float walk, float shot, GunPose* out);
}
