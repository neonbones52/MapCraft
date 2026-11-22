#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc::d3d
{
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // false with "<what> failed: 0x..." in `error` when hr failed.
    bool check(HRESULT hr, const char* what, std::string* error);

    // An immutable structured buffer of `count` elements of `stride` bytes (at least one element) and its view.
    bool structured(ID3D11Device* device, const void* data, std::size_t count, std::size_t stride,
                    ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error);

    template <class T>
    bool structured(ID3D11Device* device, const std::vector<T>& data, ComPtr<ID3D11ShaderResourceView>* srv,
                    const char* what, std::string* error)
    {
        const T none{};
        return structured(device, data.empty() ? &none : static_cast<const void*>(data.data()),
                          std::max<std::size_t>(data.size(), 1), sizeof(T), srv, what, error);
    }

    // A dynamic structured buffer of `count` elements of `stride` bytes, written with Map(WRITE_DISCARD).
    bool dynamic_structured(ID3D11Device* device, std::size_t count, std::size_t stride, ComPtr<ID3D11Buffer>* buffer,
                            ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error);

    // A texture array of square RGBA8 layers (`size` px, rows top down, layer after layer) with every mip down to
    // 1 px: 2 x 2 boxes, colours weighted by alpha so cut-out edges keep their colour.
    bool texture_array(ID3D11Device* device, const std::vector<std::uint8_t>& layers, std::uint32_t layer_count,
                       std::uint32_t size, ComPtr<ID3D11ShaderResourceView>* srv, const char* what, std::string* error);

    // Minecraft's sampling: crisp texels, blended mips, wrapping.
    bool point_sampler(ID3D11Device* device, ComPtr<ID3D11SamplerState>* out, std::string* error);
}
