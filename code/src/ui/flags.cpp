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

    ID3D11ShaderResourceView* FlagCache::texture(const CountryDefinitions& countries, std::uint16_t country, float size)
    {
        const auto px = static_cast<std::uint32_t>(std::lround(size));
        const std::uint64_t key = std::uint64_t{ country } << 32 | px;
        auto it = textures_.find(key);
        if (it == textures_.end()) {
            const auto path = dir_ / (countries.tag_text(country) + ".png");
            it = textures_.emplace(key, device_ ? load_texture(device_.Get(), path, px, px) : nullptr).first;
        }
        return it->second.Get();
    }

    void FlagCache::draw(const CountryDefinitions& countries, std::uint16_t country, float size)
    {
        if (ID3D11ShaderResourceView* flag = texture(countries, country, size)) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(flag)), ImVec2(size, size));
            return;
        }
        draw_at(ImGui::GetWindowDrawList(), countries, country, ImGui::GetCursorScreenPos(), size);
        ImGui::Dummy(ImVec2(size, size));
    }

    void FlagCache::draw_at(ImDrawList* list, const CountryDefinitions& countries, std::uint16_t country,
                            const ImVec2& at, float size, float alpha)
    {
        const ImVec2 end(at.x + size, at.y + size);
        if (ID3D11ShaderResourceView* flag = texture(countries, country, size)) {
            list->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(flag)), at, end, ImVec2(0, 0), ImVec2(1, 1),
                           ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
            return;
        }
        const float rounding = size * 0.08f;
        list->AddRectFilled(at, end, ImGui::GetColorU32(ui::color(countries.color[country], alpha)), rounding);
        list->AddRect(at, end, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f * alpha)), rounding);
    }
}
