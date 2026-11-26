cmake_minimum_required(VERSION 3.28)

project(MapCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

set(MC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# The simulation (docs/SIMULATION.md): core/ (CSV, the map's and the game's data, det_math) and game/, strictly
# deterministic, no graphics, no UI. The game links it; mapcraft_sim links it alone.
add_library(mc_sim STATIC
    src/core/csv.cpp
    src/core/log.cpp
    src/core/map_data.cpp
    src/core/map_file.cpp
    src/game/armies.cpp
    src/game/calendar.cpp
    src/game/combat.cpp
    src/game/commands.cpp
    src/game/countries.cpp
    src/game/economy/ledger.cpp
    src/game/game_data.cpp
    src/game/modifiers.cpp
    src/game/movement.cpp
    src/game/session.cpp
    src/game/world.cpp
)
target_include_directories(mc_sim PUBLIC src)
# Determinism (CLAUDE.md, rule 1): precise floating point with no contraction into FMA (MSVC's /fp:precise does not
# contract), and the default x64 instruction set (SSE2) with no /arch: one set for every CPU.
target_compile_options(mc_sim PRIVATE /fp:precise)

# The linter of mc_sim's sources (tools/lint/sim_lint.py): runs on every build before mc_sim compiles.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(mc_sim_lint
    COMMAND Python3::Interpreter -I "${MC_ROOT}/tools/lint/sim_lint.py" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    COMMENT "sim_lint: mc_sim sources"
    VERBATIM)
add_dependencies(mc_sim mc_sim_lint mc_layers_lint)

# The linter of the layer graph over all of src (tools/lint/layers.py): runs on every build, before mc_sim and mc_game
# compile.
add_custom_target(mc_layers_lint
    COMMAND Python3::Interpreter -I "${MC_ROOT}/tools/lint/layers.py" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    COMMENT "layers: the layer graph of src"
    VERBATIM)

# The view (view/), Minecraft's assets, the renderer, the map and the UI: no window, no platform. The hosts below
# feed input and a D3D11 target.
add_library(mc_game STATIC
    src/view/camera.cpp
    src/view/labels.cpp
    src/view/map_view.cpp
    src/minecraft/assets.cpp
    src/minecraft/nbt.cpp
    src/minecraft/blocks.cpp
    src/minecraft/voxels.cpp
    src/minecraft/entity_models.cpp
    src/render/map_renderer.cpp
    src/render/terrain.cpp
    src/render/models.cpp
    src/render/figures.cpp
    src/render/d3d_util.cpp
    src/render/labels.cpp
    src/render/shaders.cpp
    src/app/app.cpp
    src/app/map_modes.cpp
    src/app/terrain_look.cpp
    src/app/biome_table.cpp
    src/app/scenery.cpp
    src/app/army_figures.cpp
    src/app/army_orders.cpp
    src/app/shell.cpp
    src/app/bindings.cpp
    src/ui/imgui_layer.cpp
    src/ui/image.cpp
    src/ui/flags.cpp
    src/ui/widgets.cpp
    src/ui/game_ui.cpp
    src/ui/screens/country_select.cpp
    src/ui/screens/army_plates.cpp
    src/ui/screens/player_hud.cpp
    src/ui/screens/province_tooltip.cpp
    src/ui/screens/time_bar.cpp
)
find_package(imgui CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)
target_include_directories(mc_game PUBLIC src)
target_include_directories(mc_game PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(mc_game PUBLIC mc_sim d3d11 dxgi d3dcompiler imgui::imgui)
add_dependencies(mc_game mc_layers_lint)
target_link_libraries(mc_game PRIVATE miniz::miniz nlohmann_json::nlohmann_json)

# The Win32 platform both exes share (host/win32): the window and its input, the D3D11 device and swap chain, the log
# file.
add_library(mc_host_win32 STATIC
    src/host/win32/window.cpp
    src/host/win32/d3d_target.cpp
    src/host/win32/log_file.cpp
)
target_link_libraries(mc_host_win32 PUBLIC mc_sim d3d11 dxgi)
add_dependencies(mc_host_win32 mc_layers_lint)

# The game: a plain window with its own swap chain.
add_executable(mapcraft WIN32 src/host/win32/main.cpp)
target_link_libraries(mapcraft PRIVATE mc_game mc_host_win32)
target_compile_definitions(mapcraft PRIVATE MC_DEFAULT_MAP="${MC_ROOT}/build/map/world.mcmap"
    MC_DEFAULT_DATA="${MC_ROOT}/data" MC_DEFAULT_FLAGS="${MC_ROOT}/assets/flags" MC_DEFAULT_FONTS="${MC_ROOT}/fonts")

# The headless simulator (docs/SIMULATION.md, "Headless-симулятор"): mc_sim alone, no window, no graphics.
add_executable(mapcraft_sim
    src/headless/main.cpp
    src/headless/run_record.cpp
    src/headless/scenario.cpp
    src/headless/snapshot.cpp
)
target_link_libraries(mapcraft_sim PRIVATE mc_sim)
add_dependencies(mapcraft_sim mc_layers_lint)
target_compile_definitions(mapcraft_sim PRIVATE MC_DEFAULT_MAP="${MC_ROOT}/build/map/world.mcmap"
    MC_DEFAULT_DATA="${MC_ROOT}/data" MC_ROOT="${MC_ROOT}")

# Province editor (docs/GAMEDATA.md): the map with Dear ImGui panels over it, edits data/setup.
add_executable(mapcraft_editor WIN32 src/editor/main.cpp src/editor/editor.cpp src/editor/editor_armies.cpp)
target_link_libraries(mapcraft_editor PRIVATE mc_game mc_host_win32 imgui::imgui)
target_compile_definitions(mapcraft_editor PRIVATE MC_DEFAULT_MAP="${MC_ROOT}/build/map/world.mcmap"
    MC_DEFAULT_DATA="${MC_ROOT}/data")
