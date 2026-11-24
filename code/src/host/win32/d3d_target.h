#pragma once

#include <string>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace mc::win32
{
    // The D3D11 device of a window and its swap chain (flip discard, two BGRA8 buffers, vsync): the render target of
    // each frame, rebuilt when the window's size changes.
    class D3DTarget
    {
    public:
        bool create(HWND hwnd, std::string* error);
        // The back buffer's view at `width` x `height` px (the swap chain is resized first when that changed).
        ID3D11RenderTargetView* begin_frame(int width, int height);
        void present() { swap_chain_->Present(1, 0); }

        [[nodiscard]] ID3D11Device* device() const { return device_.Get(); }
        [[nodiscard]] ID3D11DeviceContext* context() const { return context_.Get(); }

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGISwapChain1> swap_chain_;
        ComPtr<ID3D11RenderTargetView> view_;
        int width_ = 0;
        int height_ = 0;
    };
}
