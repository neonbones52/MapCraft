#include "render/d3d_util.h"

#include <algorithm>
#include <bit>
#include <format>

namespace mc::d3d
{
    namespace
    {
        std::vector<std::uint8_t> half(const std::uint8_t* src, std::uint32_t size)
        {
            const std::uint32_t n = size / 2;
            std::vector<std::uint8_t> out(std::size_t{ n } * n * 4);
            for (std::uint32_t y = 0; y < n; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    std::uint32_t sum[3] = {};
                    std::uint32_t alpha = 0;
                    for (std::uint32_t k = 0; k < 4; ++k) {
                        const std::uint8_t* p = src + ((std::size_t{ y } * 2 + k / 2) * size + x * 2 + k % 2) * 4;
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += p[c] * (p[3] + 1u);
                        }
                        alpha += p[3];
                    }
                    std::uint8_t* q = &out[(std::size_t{ y } * n + x) * 4];
                    const std::uint32_t weight = alpha + 4;
                    for (int c = 0; c < 3; ++c) {
                        q[c] = static_cast<std::uint8_t>(sum[c] / weight);
                    }
                    q[3] = static_cast<std::uint8_t>(alpha / 4);
                }
            }
            return out;
        }
    }

    bool check(HRESULT hr, const char* what, std::string* error)
    {
        if (FAILED(hr) && error) {
            *error = std::format("{} failed: 0x{:08X}", what, static_cast<std::uint32_t>(hr));
        }
        return SUCCEEDED(hr);
    }

    bool structured(ID3D11Device* device, const void* data, std::size_t count, std::size_t stride,
                    ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(count * stride);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = static_cast<UINT>(stride);
        const D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        return check(device->CreateBuffer(&desc, &init, &buffer), what, error) &&
               check(device->CreateShaderResourceView(buffer.Get(), nullptr, srv->ReleaseAndGetAddressOf()), what,
                     error);
    }

    bool dynamic_structured(ID3D11Device* device, std::size_t count, std::size_t stride, ComPtr<ID3D11Buffer>* buffer,
                            ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(std::max<std::size_t>(count, 1) * stride);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = static_cast<UINT>(stride);
        return check(device->CreateBuffer(&desc, nullptr, buffer->ReleaseAndGetAddressOf()), what, error) &&
               check(device->CreateShaderResourceView(buffer->Get(), nullptr, srv->ReleaseAndGetAddressOf()), what,
                     error);
    }

    bool texture_array(ID3D11Device* device, const std::vector<std::uint8_t>& layers, std::uint32_t layer_count,
                       std::uint32_t size, ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error)
    {
        const auto mips = static_cast<std::uint32_t>(std::bit_width(size));   // size, size / 2, ..., 1
        std::vector<std::vector<std::uint8_t>> levels;
        levels.reserve(std::size_t{ layer_count } * mips);
        for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
            const std::uint8_t* base = &layers[std::size_t{ layer } * size * size * 4];
            levels.emplace_back(base, base + std::size_t{ size } * size * 4);
            for (std::uint32_t m = 1, s = size; m < mips; ++m, s /= 2) {
                levels.push_back(half(levels.back().data(), s));
            }
        }
        std::vector<D3D11_SUBRESOURCE_DATA> init;   // subresource = mip + layer * mips
        for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
            for (std::uint32_t m = 0; m < mips; ++m) {
                init.push_back({ levels[std::size_t{ layer } * mips + m].data(), (size >> m) * 4, 0 });
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = mips;
        desc.ArraySize = layer_count;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> array;
        return check(device->CreateTexture2D(&desc, init.data(), &array), what, error) &&
               check(device->CreateShaderResourceView(array.Get(), nullptr, srv->ReleaseAndGetAddressOf()), what,
                     error);
    }

    bool point_sampler(ID3D11Device* device, ComPtr<ID3D11SamplerState>* out, std::string* error)
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        return check(device->CreateSamplerState(&sd, out->ReleaseAndGetAddressOf()), "point sampler", error);
    }
}
