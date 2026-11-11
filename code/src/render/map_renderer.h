#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "core/camera.h"
#include "render/labels.h"

namespace mc
{
    class MapData;

    struct FrameView
    {
        CameraView camera;
        float time = 0.0f;
        std::uint32_t hovered = 0;
        std::uint32_t map_mode = 0;  // 0 terrain: no fill; otherwise the fills colour the land (EU4 formula)
        float mode_names_alpha = 1.0f;       // the map mode's names (countries, cultures, ...)
        float province_names_alpha = 0.0f;   // the provinces' names, instead of those when zoomed in
    };

    // RGBA per province: rgb colour, a = how strongly it covers the land (0 for water in political modes).
    struct ProvinceFill
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 0.0f;
    };

    enum class BorderStyle : std::uint32_t
    {
        none = 0,
        province = 1,
        country = 2,
        sea = 3,
    };

    // The map (docs/RENDER.md): for now a flat plane coloured per pixel class (sea, land, lake, wasteland), the
    // political colour applied in its pixel shader as in EU4, rivers as strips, borders as strips along smoothed
    // province outlines, and the map names. The voxel surface is to replace the plane. Sets every piece of pipeline
    // state it uses, so it can run on a context whose state was cleared; keeps its own depth buffer.
    class MapRenderer
    {
    public:
        bool init(ID3D11Device* device, const MapData& map, std::string* error);
        // Map mode data: one fill per province id, one style per border chain (MapData::border_chains order).
        void set_colors(ID3D11DeviceContext* context, std::span<const ProvinceFill> fills,
                        std::span<const BorderStyle> styles);
        // Selected provinces (editor), one flag per province id; drawn brighter and hatched.
        void set_selection(ID3D11DeviceContext* context, std::span<const std::uint8_t> selected);
        void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, const FrameView& view);
        // Map names of the current map mode (core/labels.h).
        void set_labels(const MapData& map, const LabelFont& font, std::span<const MapLabel> labels);
        void set_province_labels(const MapData& map, const LabelFont& font, std::span<const MapLabel> labels);

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        bool create_shaders(std::string* error);
        bool create_borders(const MapData& map, std::string* error);
        bool create_rivers(const MapData& map, std::string* error);
        bool create_states(std::string* error);
        bool ensure_depth(ID3D11RenderTargetView* target, UINT* width, UINT* height);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11VertexShader> map_vs_, river_vs_, border_vs_;
        ComPtr<ID3D11PixelShader> map_ps_, river_ps_, border_ps_;
        ComPtr<ID3D11Buffer> frame_constants_;

        ComPtr<ID3D11ShaderResourceView> province_ids_, classes_;
        ComPtr<ID3D11Buffer> fills_buffer_;
        ComPtr<ID3D11ShaderResourceView> fills_;
        ComPtr<ID3D11Buffer> selection_buffer_;
        ComPtr<ID3D11ShaderResourceView> selection_;
        LabelLayer labels_;
        LabelLayer province_labels_;

        ComPtr<ID3D11ShaderResourceView> river_vertices_;
        ComPtr<ID3D11Buffer> river_indices_;
        UINT river_index_count_ = 0;

        ComPtr<ID3D11ShaderResourceView> border_vertices_;
        ComPtr<ID3D11Buffer> border_indices_;
        ComPtr<ID3D11Buffer> border_styles_buffer_;
        ComPtr<ID3D11ShaderResourceView> border_styles_;
        ComPtr<ID3D11Buffer> border_pass_[2];
        UINT border_index_count_ = 0;
        std::uint32_t chain_count_ = 0;
        std::uint32_t province_count_ = 0;

        ComPtr<ID3D11SamplerState> linear_;
        ComPtr<ID3D11RasterizerState> rasterizer_;
        ComPtr<ID3D11BlendState> opaque_, alpha_;
        ComPtr<ID3D11DepthStencilState> depth_write_, depth_read_, depth_border_;
        ComPtr<ID3D11Texture2D> depth_texture_;
        ComPtr<ID3D11DepthStencilView> depth_;
        UINT depth_width_ = 0;
        UINT depth_height_ = 0;

        float map_width_ = 0.0f;
        float map_height_ = 0.0f;
    };
}
