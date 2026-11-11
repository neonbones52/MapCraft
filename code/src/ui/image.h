#pragma once

#include <cstdint>
#include <filesystem>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc
{
    // A PNG (any format WIC reads) as a straight-alpha RGBA8 texture, scaled to `width` x `height` px (0 = as is,
    // high-quality cubic when scaled); null on failure.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> load_texture(ID3D11Device* device, const std::filesystem::path& path,
                                                                  std::uint32_t width = 0, std::uint32_t height = 0);
}
