#include "host/win32/d3d_target.h"

namespace mc::win32
{
    bool D3DTarget::create(HWND hwnd, std::string* error)
    {
        UINT flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION,
                                     &device_, nullptr, &context_))) {
            *error = "D3D11CreateDevice failed (a GPU with Direct3D 11 is needed)";
            return false;
        }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            *error = "no DXGI factory for the D3D11 device";
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr, &swap_chain_))) {
            *error = "CreateSwapChainForHwnd failed";
            return false;
        }
        return true;
    }

    ID3D11RenderTargetView* D3DTarget::begin_frame(int width, int height)
    {
        if (!view_ || width != width_ || height != height_) {
            view_.Reset();
            swap_chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
            ComPtr<ID3D11Texture2D> back;
            swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back));
            device_->CreateRenderTargetView(back.Get(), nullptr, &view_);
            width_ = width;
            height_ = height;
        }
        return view_.Get();
    }
}
