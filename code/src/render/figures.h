#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mc
{
    // Animated figures on the map (the armies, app/army_figures.h): box models whose parts move, prepared on the CPU
    // (textures, meshes); each frame the caller gives every drawn figure and its parts' matrices. The renderer knows
    // no files, entities or armies.
    struct FigureSet
    {
        static constexpr std::uint32_t kTextureSize = 64;   // px per layer side

        // A triangle-list vertex in its part's space (the part's matrix takes it to the world).
        struct Vertex
        {
            float pos[3];
            float normal[3];
            float uv[2];
            std::uint32_t layer_part;   // texture layer | part << 16
        };
        static_assert(sizeof(Vertex) == 36);
        struct Model
        {
            std::uint32_t first = 0;   // vertex
            std::uint32_t count = 0;
        };

        std::vector<std::uint8_t> layers;   // layer_count * kTextureSize^2 RGBA8
        std::uint32_t layer_count = 0;
        std::vector<Vertex> vertices;
        std::vector<Model> models;
    };

    // One part's matrix: world = m * (x, y, z, 1), rows.
    struct FigureMatrix
    {
        float m[3][4];
    };
    // A figure to draw: its model and where its parts' matrices start (one per part of the model, in part order).
    struct FigureDraw
    {
        std::uint32_t model = 0;
        std::uint32_t first_matrix = 0;
    };

    // Draws the figures over the map and its names, depth-tested against the map: Minecraft's directional shade by
    // each face's turned normal, texels under half alpha cut out, the fog at the map edges; fading in or out, they
    // dissolve in the models' ordered dither.
    class FigureLayer
    {
    public:
        bool init(ID3D11Device* device, const FigureSet& set, std::string* error);
        // The map renderer has set the frame constants and depth. `fade`: 0 hidden, 1 whole.
        void draw(ID3D11DeviceContext* context, std::span<const FigureDraw> figures,
                  std::span<const FigureMatrix> matrices, float fade);

    private:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        bool reserve(std::size_t figures, std::size_t matrices);

        ComPtr<ID3D11Device> device_;
        std::vector<FigureSet::Model> models_;
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11ShaderResourceView> vertices_;
        ComPtr<ID3D11ShaderResourceView> layers_;
        ComPtr<ID3D11Buffer> instance_buffer_;   // per figure: its first matrix, grouped by model
        ComPtr<ID3D11ShaderResourceView> instance_view_;
        ComPtr<ID3D11Buffer> matrix_buffer_;
        ComPtr<ID3D11ShaderResourceView> matrix_view_;
        std::size_t instance_capacity_ = 0;
        std::size_t matrix_capacity_ = 0;
        ComPtr<ID3D11Buffer> draw_constants_;
        ComPtr<ID3D11SamplerState> point_;
        std::vector<std::uint32_t> by_model_;     // scratch: first matrices, grouped by model
        std::vector<std::uint32_t> model_first_;
        std::vector<std::uint32_t> model_count_;
    };
}
