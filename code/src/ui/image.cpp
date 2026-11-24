#include "ui/image.h"

#include "core/log.h"
#include "minecraft/assets.h"

namespace mc
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> load_texture(ID3D11Device* device, const std::filesystem::path& path,
                                                                  std::uint32_t width, std::uint32_t height)
    {
        if (!std::filesystem::exists(path)) {
            return nullptr;
        }
        minecraft::Image image;
        if (!minecraft::read_png(path, &image)) {
            log::warn("cannot read image {}", path.string());
            return nullptr;
        }
        if (width > 0 && height > 0 && (width != image.width || height != image.height)) {
            image = minecraft::resized(image, width, height);
            if (image.rgba.empty()) {
                log::warn("cannot scale image {}", path.string());
                return nullptr;
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width;
        desc.Height = image.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{ image.rgba.data(), image.width * 4, 0 };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
            log::warn("cannot create a texture for {}", path.string());
            return nullptr;
        }
        return view;
    }
}
