cmake_minimum_required(VERSION 3.28)

project(MapCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

set(MC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# Game core + renderer + UI: no window, no platform. The hosts below feed input and a D3D11 target.
add_library(mc_game STATIC
    src/core/camera.cpp
    src/core/csv.cpp
    src/game/game_data.cpp
    src/core/log.cpp
    src/core/map_data.cpp
    src/core/labels.cpp
    src/minecraft/assets.cpp
    src/render/map_renderer.cpp
    src/render/terrain.cpp
    src/render/labels.cpp
    src/render/shaders.cpp
    src/app/app.cpp
    src/app/map_modes.cpp
    src/app/terrain_look.cpp
    src/app/shell.cpp
    src/app/bindings.cpp
    src/game/calendar.cpp
    src/game/countries.cpp
    src/game/session.cpp
    src/game/world.cpp
    src/ui/imgui_layer.cpp
    src/ui/image.cpp
    src/ui/flags.cpp
    src/ui/widgets.cpp
    src/ui/game_ui.cpp
    src/ui/screens/country_select.cpp
    src/ui/screens/player_hud.cpp
    src/ui/screens/time_bar.cpp
)
find_package(imgui CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)
target_include_directories(mc_game PUBLIC src)
target_include_directories(mc_game PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(mc_game PUBLIC d3d11 dxgi d3dcompiler windowscodecs imgui::imgui)
target_link_libraries(mc_game PRIVATE miniz::miniz nlohmann_json::nlohmann_json)

# The game: a plain window with its own swap chain.
add_executable(mapcraft WIN32 src/host/win32/main.cpp)
target_link_libraries(mapcraft PRIVATE mc_game)
target_compile_definitions(mapcraft PRIVATE MC_DEFAULT_MAP="${MC_ROOT}/build/map/world.mcmap"
    MC_DEFAULT_DATA="${MC_ROOT}/data" MC_DEFAULT_FLAGS="${MC_ROOT}/assets/flags")

# Province editor (docs/GAMEDATA.md): the map with Dear ImGui panels over it, edits data/setup.
add_executable(mapcraft_editor WIN32 src/editor/main.cpp src/editor/editor.cpp)
target_link_libraries(mapcraft_editor PRIVATE mc_game imgui::imgui)
target_compile_definitions(mapcraft_editor PRIVATE MC_DEFAULT_MAP="${MC_ROOT}/build/map/world.mcmap"
    MC_DEFAULT_DATA="${MC_ROOT}/data")
