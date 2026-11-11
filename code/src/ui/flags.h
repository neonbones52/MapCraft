#pragma once

#include <cstdint>
#include <filesystem>
#include <map>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc
{
    struct CountryDefinitions;

    // Country flags as ImGui images: <dir>/<TAG>.png (assets/flags), each
    // loaded once per drawn size (scaled down by WIC, so it is crisp 1:1). A country without a flag file is drawn as
    // a square of its colour.
    class FlagCache
    {
    public:
        void init(ID3D11Device* device, std::filesystem::path dir);
        // An ImGui item of size x size px at the cursor.
        void draw(const CountryDefinitions& countries, std::uint16_t country, float size);

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        std::filesystem::path dir_;
        std::map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> textures_;   // country << 32 | px
    };
}
