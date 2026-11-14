#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

#include "core/camera.h"
#include "core/labels.h"

namespace mc
{
    class MapData;

    // The map names (core/labels.h) as glyph quads lying on the blocks: every name is one flat plane at the median top
    // (ground or water) of the blocks under its letters, so it never steps. A name's pixel is hidden where blocks of
    // other groups' provinces stand between it and the eye (a ray march in the pixel shader, docs/RENDER.md); its own
    // land never hides it. Drawn from the SDF atlas of
    // tools/fonts/mapfont.py in two passes, every halo first and then every ink, the look of archeologyEU4/Labels/LabelPython/rendering/text.py:
    // ink RGB 32,30,26 (text.py 42,40,36, darkened by the user); a Gaussian halo (sigma = max(0.3, 0.055 x ink height)
    // map px, alpha x1.65 capped at 199/255) in RGB 180,180,180 (the user's).
    class LabelLayer
    {
    public:
        bool init(ID3D11Device* device, const MapData& map, std::string* error);
        // `keys`: per province id the group its names belong to (the province itself for province names; owner,
        // region, culture or religion for a map mode's names), 0 = none. A name's group is the most common key under
        // its glyphs; blocks of provinces of that group never hide it.
        void set_labels(ID3D11Device* device, const MapData& map, const LabelFont& font,
                        std::span<const MapLabel> labels, std::span<const std::uint32_t> keys);
        // Needs the renderer's state (frame constants b0, linear clamp sampler s0, the terrain's block data t4 and
        // constants b2) and an alpha blend state.
        void draw(ID3D11DeviceContext* context, float alpha);

        [[nodiscard]] std::size_t glyph_count() const { return glyph_count_; }

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        // World height of a name's plane.
        [[nodiscard]] static float plane_height(const MapData& map, const MapLabel& label);
        [[nodiscard]] static std::uint32_t label_group(const MapData& map, const MapLabel& label,
                                                       std::span<const std::uint32_t> keys);

        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> halo_ps_;
        ComPtr<ID3D11PixelShader> ink_ps_;
        ComPtr<ID3D11ShaderResourceView> atlas_;
        ComPtr<ID3D11ShaderResourceView> vertices_;
        ComPtr<ID3D11ShaderResourceView> keys_;          // t18: per province its group key
        ComPtr<ID3D11Buffer> constants_;
        ComPtr<ID3D11DepthStencilState> no_depth_;
        std::size_t glyph_count_ = 0;
        float atlas_size_[2] = { 1.0f, 1.0f };
        float spread_ = 1.0f;
    };
}
