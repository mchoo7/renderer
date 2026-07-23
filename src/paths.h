#pragma once

#include <filesystem>
#include <string_view>

#ifndef PROJECT_ROOT_DIR
#error "PROJECT_ROOT_DIR must be defined by CMake (see target_compile_definitions in CMakeLists.txt)"
#endif

// Resolves a path relative to the repo root, e.g. assetPath("assets/models/cube.gltf").
inline std::filesystem::path assetPath(std::string_view relative) {
    return std::filesystem::path(PROJECT_ROOT_DIR) / relative;
}
