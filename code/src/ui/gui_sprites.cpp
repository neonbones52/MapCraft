#include "ui/gui_sprites.h"

#include <string>

#include "core/log.h"
#include "ui/image.h"

namespace mc
{
    namespace
    {
        // In Sprite's order: textures/gui/sprites/<name>.png.
        constexpr std::string_view kSpriteNames[] = {
            "widget/button",
            "widget/button_highlighted",
            "widget/button_disabled",
            "widget/slider",
            "widget/slider_handle",
            "widget/slider_handle_highlighted",
            "widget/checkbox",
            "widget/checkbox_selected",
            "widget/tab",
            "widget/tab_highlighted",
            "widget/tab_selected",
            "widget/tab_selected_highlighted",
            "widget/scroller",
            "widget/scroller_background",
            "tooltip/background",
            "tooltip/frame",
            "widget/cross_button",
            "widget/cross_button_highlighted",
            "container/slot",
            "boss_bar/red_background",
            "boss_bar/red_progress",
            "boss_bar/green_background",
            "boss_bar/green_progress",
            "boss_bar/yellow_background",
            "boss_bar/yellow_progress",
            "boss_bar/blue_background",
            "boss_bar/blue_progress",
            "boss_bar/white_background",
            "boss_bar/white_progress",
            "",   // panel: made here
            "",   // inset: made here
        };
        static_assert(std::size(kSpriteNames) == static_cast<std::size_t>(Sprite::count), "a name per sprite");

        // The panel, 13 x 13 GUI px, nine-sliced 6 px, from the corners of gui/container/generic_54.png: K the outline,
        // W the light, G the grey, D the shade, . nothing.
        constexpr const char* kPanel[13] = {
            "..KKKKKKKK...",
            ".KWWWWWWWWK..",
            "KWWWWWWWWWGK.",
            "KWWWGGGGGGDDK",
            "KWWGGGGGGGDDK",
            "KWWGGGGGGGDDK",
            "KWWGGGGGGGDDK",
            "KWWGGGGGGGDDK",
            "KWWGGGGGGGDDK",
            "KWWGGGGGGDDDK",
            ".KGDDDDDDDDDK",
            "..KDDDDDDDDK.",
            "...KKKKKKKK..",
        };
        // The inset, 3 x 3 GUI px, nine-sliced 1 px (gui/sprites/container/slot.png's edges): I the dark, W the light,
        // S the slot's grey.
        constexpr const char* kInset[3] = {
            "IIS",
            "ISW",
            "SWW",
        };

        std::uint32_t pixel_of(char c)
        {
            switch (c) {   // 0xAABBGGRR
            case 'K':
                return 0xFF000000u;
            case 'W':
                return 0xFFFFFFFFu;
            case 'G':
                return 0xFFC6C6C6u;
            case 'D':
                return 0xFF555555u;
            case 'I':
                return 0xFF373737u;
            case 'S':
                return 0xFF8B8B8Bu;
            default:
                return 0;
            }
        }

        // An image from rows of letters, nine-sliced `border` px with the middle stretched (a flat colour).
        template <std::size_t N>
        void made(const char* const (&rows)[N], std::uint32_t border, minecraft::Image* image,
                  minecraft::GuiScaling* scaling)
        {
            image->width = static_cast<std::uint32_t>(std::char_traits<char>::length(rows[0]));
            image->height = static_cast<std::uint32_t>(N);
            image->rgba.assign(std::size_t{ image->width } * image->height * 4, 0);
            for (std::uint32_t y = 0; y < image->height; ++y) {
                for (std::uint32_t x = 0; x < image->width; ++x) {
                    const std::uint32_t p = pixel_of(rows[y][x]);
                    for (int c = 0; c < 4; ++c) {
                        image->rgba[(std::size_t{ y } * image->width + x) * 4 + c] =
                            static_cast<std::uint8_t>(p >> (8 * c));
                    }
                }
            }
            *scaling = minecraft::GuiScaling{};
            scaling->kind = minecraft::GuiScaling::Kind::nine_slice;
            scaling->width = image->width;
            scaling->height = image->height;
            scaling->left = scaling->top = scaling->right = scaling->bottom = border;
            scaling->stretch_inner = true;
        }

        SpriteImage upload(ID3D11Device* device, const minecraft::Image& image, const minecraft::GuiScaling& scaling)
        {
            SpriteImage out;
            out.texture = make_texture(device, image);
            out.width = image.width;
            out.height = image.height;
            out.scaling = scaling;
            return out;
        }
    }

    GuiSprites::GuiSprites() = default;
    GuiSprites::~GuiSprites() = default;

    bool GuiSprites::load(ID3D11Device* device, const std::filesystem::path& jar)
    {
        device_ = device;
        auto assets = std::make_unique<minecraft::Assets>();
        std::string error;
        if (jar.empty() || !assets->open(jar, &error)) {
            log::warn("gui sprites: no Minecraft jar ({}): the widgets draw without them", error);
            return false;
        }
        std::size_t missing = 0;
        for (std::size_t i = 0; i < std::size(kSpriteNames); ++i) {
            minecraft::Image image;
            minecraft::GuiScaling scaling;
            if (static_cast<Sprite>(i) == Sprite::panel) {
                made(kPanel, 6, &image, &scaling);
            } else if (static_cast<Sprite>(i) == Sprite::inset) {
                made(kInset, 1, &image, &scaling);
            } else if (!assets->gui_sprite(kSpriteNames[i], &image, &scaling, &error)) {
                log::warn("gui sprites: {}", error);
                ++missing;
                continue;
            }
            if (kSpriteNames[i].starts_with("boss_bar/") && scaling.kind == minecraft::GuiScaling::Kind::stretch) {
                // No .mcmeta: cut in three so that a bar takes any width without stretching its pixels.
                scaling.kind = minecraft::GuiScaling::Kind::nine_slice;
                scaling.left = scaling.right = 2;
            }
            sprites_[i] = upload(device, image, scaling);
        }
        log::info("gui sprites: {} from {}, {} missing", std::size(kSpriteNames) - missing, jar.string(), missing);
        assets_ = std::move(assets);
        return true;
    }

    const SpriteImage* GuiSprites::item(std::string_view name)
    {
        if (const auto found = items_.find(name); found != items_.end()) {
            return found->second.texture ? &found->second : nullptr;
        }
        SpriteImage& slot = items_[std::string(name)];
        if (!assets_) {
            return nullptr;
        }
        minecraft::Image image;
        if (!assets_->image("item/" + std::string(name), &image, nullptr)) {
            log::warn("gui sprites: no item icon {}", name);
            return nullptr;
        }
        minecraft::GuiScaling scaling;
        scaling.width = image.width;
        scaling.height = image.height;
        slot = upload(device_.Get(), image, scaling);
        return slot.texture ? &slot : nullptr;
    }
}
