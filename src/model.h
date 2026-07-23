#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

// CPU-side, graphics-API-agnostic model data. A renderer backend (src/renderers/*)
// consumes this to upload GPU buffers/textures however it likes (VAO/VBO, VkBuffer, etc).

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec4 tangent{0.0f}; // w = handedness
    glm::vec2 texCoord{0.0f};
};

struct ImageData {
    std::vector<std::uint8_t> pixels; // tightly packed RGBA8
    int width = 0;
    int height = 0;
};

struct MaterialData {
    glm::vec4 baseColorFactor{1.0f};
    std::optional<std::size_t> baseColorTextureIndex;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    std::optional<std::size_t> metallicRoughnessTextureIndex;
    std::optional<std::size_t> normalTextureIndex;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::optional<std::size_t> materialIndex;
};

struct MeshData {
    std::vector<Primitive> primitives;
};

struct Model {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<ImageData> images;
};

// Loads a glTF/GLB file via fastgltf, decodes images to RGBA8, and bakes each scene
// node's world transform into its mesh instance's vertices. Returns std::nullopt on failure.
std::optional<Model> loadModel(const std::filesystem::path &path);
