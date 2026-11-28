#pragma once

#include <cstdint>
#include <filesystem>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc
{
    namespace minecraft
    {
        struct Image;
    }

    // An RGBA8 image (straight alpha) as an immutable texture; null on failure.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> make_texture(ID3D11Device* device, const minecraft::Image& image);
    // A PNG file as a straight-alpha RGBA8 texture, scaled to `width` x `height` px (0 = as is; decoded and scaled by
    // minecraft/assets.h, stb); null on failure.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> load_texture(ID3D11Device* device, const std::filesystem::path& path,
                                                                  std::uint32_t width = 0, std::uint32_t height = 0);
}
