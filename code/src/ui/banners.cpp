#include "ui/banners.h"

#include <span>
#include <string>

#include "core/log.h"
#include "game/game_data.h"
#include "minecraft/banners.h"
#include "ui/image.h"

namespace mc
{
    namespace
    {
        constexpr std::uint32_t kObserver = 0xFFFFFFFFu;
        constexpr std::uint32_t kDarkGrey = 0x474F52;   // Minecraft's gray dye
        constexpr std::uint32_t kBlack = 0x1D1D21;      // its black dye

        // A banner into `slot`; an empty slot (drawn as nothing) if it cannot be made.
        const SpriteImage* make(GuiSprites* sprites, std::uint32_t base,
                                std::span<const minecraft::BannerLayer> layers, SpriteImage* slot)
        {
            if (sprites == nullptr || sprites->assets() == nullptr) {
                return nullptr;
            }
            minecraft::Image image;
            std::string error;
            if (!minecraft::compose_banner(*sprites->assets(), base, layers, &image, &error)) {
                log::warn("banner: {}", error);
                return nullptr;
            }
            slot->texture = make_texture(sprites->device(), image);
            slot->width = image.width;
            slot->height = image.height;
            slot->scaling = minecraft::GuiScaling{};
            slot->scaling.width = image.width;
            slot->scaling.height = image.height;
            return slot->texture ? slot : nullptr;
        }
    }

    const SpriteImage* BannerCache::country(const CountryDefinitions& countries, std::uint16_t country)
    {
        if (country == 0 || country >= countries.size()) {
            return observer();
        }
        if (const auto found = made_.find(country); found != made_.end()) {
            return found->second.texture ? &found->second : nullptr;
        }
        // The map colour is RGBA8 with r in the low byte.
        const std::uint32_t rgba = countries.color[country];
        const std::uint32_t rgb = ((rgba & 255) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 255);
        return make(sprites_, rgb, {}, &made_[country]);
    }

    const SpriteImage* BannerCache::observer()
    {
        if (const auto found = made_.find(kObserver); found != made_.end()) {
            return found->second.texture ? &found->second : nullptr;
        }
        const minecraft::BannerLayer half[] = { { "diagonal_up_left", kBlack } };
        return make(sprites_, kDarkGrey, half, &made_[kObserver]);
    }
}
