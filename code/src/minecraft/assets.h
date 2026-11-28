#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::minecraft
{
    // RGBA8, rows top down.
    struct Image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;

        [[nodiscard]] std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const;   // 0xAABBGGRR
    };

    // A texture as Minecraft animates it (`<name>.png.mcmeta`): square frames stacked top down, shown in `order`
    // (indices into the stack) for `frame_time` seconds each. A still texture has one frame.
    struct Texture
    {
        Image image;
        std::uint32_t frame_size = 0;          // frame side, px (the image width)
        std::vector<std::uint32_t> order;
        float frame_time = 0.05f;
    };

    // How a GUI sprite is drawn at another size (`textures/gui/sprites/<name>.png.mcmeta`, "gui" / "scaling"):
    // stretched (the default), tiled, or nine-sliced (the corners as they are, the edges and the middle tiled, or
    // stretched with stretch_inner). `width` x `height` is the size the tiles and the slices are laid out at, in GUI
    // pixels (the image's own without .mcmeta).
    struct GuiScaling
    {
        enum class Kind : std::uint8_t
        {
            stretch,
            tile,
            nine_slice,
        };
        Kind kind = Kind::stretch;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t left = 0;     // the nine slices' borders
        std::uint32_t top = 0;
        std::uint32_t right = 0;
        std::uint32_t bottom = 0;
        bool stretch_inner = false;
    };

    // What a vanilla biome (data/minecraft/worldgen/biome/<name>.json) says about colours. Optional colours override
    // the colour maps.
    struct Biome
    {
        float temperature = 0.5f;
        float downfall = 0.5f;
        std::optional<std::uint32_t> grass_color;     // 0xRRGGBB
        std::optional<std::uint32_t> foliage_color;
        std::uint32_t water_color = 0x3F76E4;
        std::string grass_modifier;                   // "", "swamp", "dark_forest"
    };

    // The player's Minecraft client jar (a zip): textures, .mcmeta, colour maps and the vanilla data pack. Nothing of
    // it is copied anywhere: it is read while the game runs (CLAUDE.md, "Ассеты Майнкрафта").
    class Assets
    {
    public:
        Assets();
        ~Assets();
        Assets(const Assets&) = delete;
        Assets& operator=(const Assets&) = delete;

        bool open(const std::filesystem::path& jar, std::string* error);
        [[nodiscard]] const std::filesystem::path& path() const { return path_; }

        // `name` is relative to assets/minecraft/textures without .png ("block/stone", "colormap/grass").
        [[nodiscard]] bool has_texture(std::string_view name) const;
        bool texture(std::string_view name, Texture* out, std::string* error) const;
        bool image(std::string_view name, Image* out, std::string* error) const;
        bool biome(std::string_view name, Biome* out, std::string* error) const;
        // A GUI sprite, `name` relative to textures/gui/sprites without .png ("widget/button"), and its scaling.
        bool gui_sprite(std::string_view name, Image* out, GuiScaling* scaling, std::string* error) const;

        // Any entry of the jar by its full path ("data/minecraft/structure/....nbt"); false if there is none.
        bool file(std::string_view entry, std::vector<std::uint8_t>* out) const;
        // The entries (files, not folders) whose path starts with `prefix`, sorted.
        [[nodiscard]] std::vector<std::string> list(std::string_view prefix) const;

    private:

        struct Zip;
        std::unique_ptr<Zip> zip_;
        std::filesystem::path path_;
    };

    // Where a Minecraft 26.2 client jar is looked for, in this order: TLauncher's Fabric 26.2, the official launcher's
    // 26.2 (both under %APPDATA%).
    [[nodiscard]] std::vector<std::filesystem::path> jar_candidates();
    // The first candidate that exists, else empty.
    [[nodiscard]] std::filesystem::path find_jar();

    // A PNG file on disk, outside the jar (country flags), decoded by the same stb_image as the jar's textures.
    bool read_png(const std::filesystem::path& path, Image* out);
    // `image` scaled to width x height px (stb_image_resize2: sRGB, alpha-weighted).
    [[nodiscard]] Image resized(const Image& image, std::uint32_t width, std::uint32_t height);

    // Minecraft's colour map lookup (GrassColor / FoliageColor): temperature and downfall clamped to 0..1, downfall
    // scaled by temperature, then the 256 x 256 map at ((1 - t) * 255, (1 - d) * 255). -> 0xRRGGBB.
    [[nodiscard]] std::uint32_t colormap_color(const Image& map, float temperature, float downfall);
}
