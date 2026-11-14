#include "minecraft/assets.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include <miniz.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4701)
#include <stb_image.h>
#pragma warning(pop)

namespace mc::minecraft
{
    namespace
    {
        constexpr std::string_view kTextures = "assets/minecraft/textures/";
        constexpr std::string_view kBiomes = "data/minecraft/worldgen/biome/";
        constexpr float kTick = 0.05f;   // s: Minecraft's game tick, the unit of frametime

        bool decode_png(const std::vector<std::uint8_t>& bytes, Image* out)
        {
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
            if (!pixels) {
                return false;
            }
            out->width = static_cast<std::uint32_t>(w);
            out->height = static_cast<std::uint32_t>(h);
            out->rgba.assign(pixels, pixels + std::size_t{ out->width } * out->height * 4);
            stbi_image_free(pixels);
            return true;
        }

        // "#3f76e4" or an integer (older data packs) -> 0xRRGGBB.
        std::optional<std::uint32_t> color_value(const nlohmann::json& value)
        {
            if (value.is_number_integer()) {
                return static_cast<std::uint32_t>(value.get<std::int64_t>()) & 0xFFFFFF;
            }
            if (value.is_string()) {
                const std::string text = value.get<std::string>();
                if (text.size() == 7 && text[0] == '#') {
                    return static_cast<std::uint32_t>(std::strtoul(text.c_str() + 1, nullptr, 16));
                }
            }
            return std::nullopt;
        }
    }

    std::uint32_t Image::pixel(std::uint32_t x, std::uint32_t y) const
    {
        const std::size_t i = (std::size_t{ y } * width + x) * 4;
        return rgba[i] | (rgba[i + 1] << 8) | (rgba[i + 2] << 16) | (std::uint32_t{ rgba[i + 3] } << 24);
    }

    struct Assets::Zip
    {
        mz_zip_archive archive{};
        bool open = false;

        ~Zip()
        {
            if (open) {
                mz_zip_reader_end(&archive);
            }
        }
    };

    Assets::Assets() = default;
    Assets::~Assets() = default;

    bool Assets::open(const std::filesystem::path& jar, std::string* error)
    {
        zip_ = std::make_unique<Zip>();
        path_ = jar;
        const std::string name = jar.string();
        if (!mz_zip_reader_init_file(&zip_->archive, name.c_str(), 0)) {
            zip_.reset();
            if (error) {
                *error = std::format("{}: not a readable jar", name);
            }
            return false;
        }
        zip_->open = true;
        if (!has_texture("block/stone")) {
            if (error) {
                *error = std::format("{}: no assets/minecraft/textures in it (not a full Minecraft client jar)", name);
            }
            return false;
        }
        return true;
    }

    bool Assets::read(std::string_view entry, std::vector<std::uint8_t>* out) const
    {
        if (!zip_) {
            return false;
        }
        const std::string name(entry);
        const int index = mz_zip_reader_locate_file(&zip_->archive, name.c_str(), nullptr, 0);
        if (index < 0) {
            return false;
        }
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip_->archive, static_cast<mz_uint>(index), &stat)) {
            return false;
        }
        out->resize(static_cast<std::size_t>(stat.m_uncomp_size));
        return mz_zip_reader_extract_to_mem(&zip_->archive, static_cast<mz_uint>(index), out->data(), out->size(), 0) !=
               0;
    }

    bool Assets::has_texture(std::string_view name) const
    {
        if (!zip_) {
            return false;
        }
        const std::string entry = std::format("{}{}.png", kTextures, name);
        return mz_zip_reader_locate_file(&zip_->archive, entry.c_str(), nullptr, 0) >= 0;
    }

    bool Assets::image(std::string_view name, Image* out, std::string* error) const
    {
        std::vector<std::uint8_t> bytes;
        const std::string entry = std::format("{}{}.png", kTextures, name);
        if (!read(entry, &bytes)) {
            if (error) {
                *error = std::format("{}: no {}", path_.string(), entry);
            }
            return false;
        }
        if (!decode_png(bytes, out)) {
            if (error) {
                *error = std::format("{}: {} is not a PNG stb_image can read", path_.string(), entry);
            }
            return false;
        }
        return true;
    }

    bool Assets::texture(std::string_view name, Texture* out, std::string* error) const
    {
        if (!image(name, &out->image, error)) {
            return false;
        }
        const Image& image = out->image;
        out->frame_size = image.width;
        if (image.width == 0 || image.height % image.width != 0) {
            if (error) {
                *error = std::format("{}: texture {} is {}x{}, not a stack of square frames", path_.string(), name,
                                     image.width, image.height);
            }
            return false;
        }
        const std::uint32_t frames = image.height / image.width;
        out->order.clear();
        out->frame_time = kTick;
        std::vector<std::uint8_t> meta;
        if (read(std::format("{}{}.png.mcmeta", kTextures, name), &meta)) {
            const nlohmann::json json = nlohmann::json::parse(meta.begin(), meta.end(), nullptr, false);
            if (json.is_object() && json.contains("animation") && json["animation"].is_object()) {
                const nlohmann::json& animation = json["animation"];
                if (animation.contains("frametime") && animation["frametime"].is_number()) {
                    out->frame_time = std::max(animation["frametime"].get<float>(), 1.0f) * kTick;
                }
                if (animation.contains("frames") && animation["frames"].is_array()) {
                    for (const nlohmann::json& frame : animation["frames"]) {
                        const nlohmann::json& index = frame.is_object() ? frame.value("index", nlohmann::json()) : frame;
                        if (index.is_number_integer() && index.get<std::int64_t>() >= 0 &&
                            index.get<std::int64_t>() < frames) {
                            out->order.push_back(static_cast<std::uint32_t>(index.get<std::int64_t>()));
                        }
                    }
                }
            }
        }
        if (out->order.empty()) {
            // Without an animation only the first frame shows (a stack without .mcmeta is not animated).
            const std::uint32_t shown = meta.empty() ? 1 : frames;
            for (std::uint32_t i = 0; i < shown; ++i) {
                out->order.push_back(i);
            }
        }
        return true;
    }

    bool Assets::biome(std::string_view name, Biome* out, std::string* error) const
    {
        std::vector<std::uint8_t> bytes;
        const std::string entry = std::format("{}{}.json", kBiomes, name);
        if (!read(entry, &bytes)) {
            if (error) {
                *error = std::format("{}: no vanilla biome {} ({})", path_.string(), name, entry);
            }
            return false;
        }
        const nlohmann::json json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!json.is_object()) {
            if (error) {
                *error = std::format("{}: {} is not JSON", path_.string(), entry);
            }
            return false;
        }
        *out = Biome{};
        if (json.contains("temperature") && json["temperature"].is_number()) {
            out->temperature = json["temperature"].get<float>();
        }
        if (json.contains("downfall") && json["downfall"].is_number()) {
            out->downfall = json["downfall"].get<float>();
        }
        if (json.contains("effects") && json["effects"].is_object()) {
            const nlohmann::json& effects = json["effects"];
            if (effects.contains("grass_color")) {
                out->grass_color = color_value(effects["grass_color"]);
            }
            if (effects.contains("foliage_color")) {
                out->foliage_color = color_value(effects["foliage_color"]);
            }
            if (effects.contains("water_color")) {
                out->water_color = color_value(effects["water_color"]).value_or(out->water_color);
            }
            if (effects.contains("grass_color_modifier") && effects["grass_color_modifier"].is_string()) {
                out->grass_modifier = effects["grass_color_modifier"].get<std::string>();
            }
        }
        return true;
    }

    std::vector<std::filesystem::path> jar_candidates()
    {
        std::vector<std::filesystem::path> paths;
        wchar_t* appdata = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&appdata, &length, L"APPDATA") == 0 && appdata) {
            const std::filesystem::path root(appdata);
            std::free(appdata);
            paths.push_back(root / ".tlauncher" / "legacy" / "Minecraft" / "game" / "versions" / "Fabric 26.2" /
                            "Fabric 26.2.jar");
            paths.push_back(root / ".minecraft" / "versions" / "26.2" / "26.2.jar");
        }
        return paths;
    }

    std::filesystem::path find_jar()
    {
        for (const std::filesystem::path& path : jar_candidates()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                return path;
            }
        }
        return {};
    }

    std::uint32_t colormap_color(const Image& map, float temperature, float downfall)
    {
        const float t = std::clamp(temperature, 0.0f, 1.0f);
        const float d = std::clamp(downfall, 0.0f, 1.0f) * t;
        const auto x = static_cast<std::uint32_t>((1.0f - t) * 255.0f);
        const auto y = static_cast<std::uint32_t>((1.0f - d) * 255.0f);
        if (x >= map.width || y >= map.height) {
            return 0xFF00FF;
        }
        const std::uint32_t p = map.pixel(x, y);
        return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
}
