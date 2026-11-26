#pragma once

#include <cstdint>
#include <filesystem>
#include <map>

#include <d3d11.h>
#include <wrl/client.h>

struct ImDrawList;
struct ImVec2;

namespace mc
{
    struct CountryDefinitions;

    // Country flags as ImGui images: <dir>/<TAG>.png (assets/flags), each
    // loaded once per drawn size (scaled down on the CPU, so it is crisp 1:1). A country without a flag file is drawn as
    // a square of its colour.
    class FlagCache
    {
    public:
        void init(ID3D11Device* device, std::filesystem::path dir);
        // An ImGui item of size x size px at the cursor.
        void draw(const CountryDefinitions& countries, std::uint16_t country, float size);
        // The same into a draw list, its top left at `at` (the army plates).
        void draw_at(ImDrawList* list, const CountryDefinitions& countries, std::uint16_t country, const ImVec2& at,
                     float size, float alpha = 1.0f);

    private:
        // The country's texture at `size` px (loaded on first use), null without a flag file.
        ID3D11ShaderResourceView* texture(const CountryDefinitions& countries, std::uint16_t country, float size);

        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        std::filesystem::path dir_;
        std::map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> textures_;   // country << 32 | px
    };
}
