#include "ui/image.h"

#include <vector>

#include <wincodec.h>

#include "core/log.h"

namespace mc
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        bool decode(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                    std::vector<std::uint8_t>* rgba, std::uint32_t* out_width, std::uint32_t* out_height)
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
                FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                             nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                return false;
            }
            UINT w = 0;
            UINT h = 0;
            converter->GetSize(&w, &h);
            IWICBitmapSource* source = converter.Get();
            ComPtr<IWICBitmapScaler> scaler;
            if (width > 0 && height > 0 && (width != w || height != h)) {
                if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
                    FAILED(scaler->Initialize(converter.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic))) {
                    return false;
                }
                source = scaler.Get();
                w = width;
                h = height;
            }
            rgba->resize(std::size_t{ w } * h * 4);
            if (FAILED(source->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba->size()), rgba->data()))) {
                return false;
            }
            *out_width = w;
            *out_height = h;
            return true;
        }
    }

    ComPtr<ID3D11ShaderResourceView> load_texture(ID3D11Device* device, const std::filesystem::path& path,
                                                  std::uint32_t width, std::uint32_t height)
    {
        if (!std::filesystem::exists(path)) {
            return nullptr;
        }
        // WIC needs COM on this thread; the host may already have initialised it (in either model).
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        const bool decoded = decode(path, width, height, &rgba, &w, &h);
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        if (!decoded) {
            log::warn("cannot read image {}", path.string());
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{ rgba.data(), w * 4, 0 };
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
            log::warn("cannot create a texture for {}", path.string());
            return nullptr;
        }
        return view;
    }
}
