#include "ui/flags.h"

#include <cmath>

#include <imgui.h>

#include "game/game_data.h"
#include "ui/image.h"
#include "ui/widgets.h"

namespace mc
{
    void FlagCache::init(ID3D11Device* device, std::filesystem::path dir)
    {
        device_ = device;
        dir_ = std::move(dir);
        textures_.clear();
    }

    void FlagCache::draw(const CountryDefinitions& countries, std::uint16_t country, float size)
    {
        const auto px = static_cast<std::uint32_t>(std::lround(size));
        const std::uint64_t key = std::uint64_t{ country } << 32 | px;
        auto it = textures_.find(key);
        if (it == textures_.end()) {
            const auto path = dir_ / (countries.tag_text(country) + ".png");
            it = textures_.emplace(key, device_ ? load_texture(device_.Get(), path, px, px) : nullptr).first;
        }
        if (it->second) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(it->second.Get())), ImVec2(size, size));
            return;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), ImGui::GetColorU32(ui::color(countries.color[country])),
                            4.0f);
        draw->AddRect(at, ImVec2(at.x + size, at.y + size), ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)), 4.0f);
        ImGui::Dummy(ImVec2(size, size));
    }
}
