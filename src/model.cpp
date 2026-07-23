#include "model.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <iostream>

namespace {

constexpr auto kGltfOptions = fastgltf::Options::DontRequireValidAssetMember |
                              fastgltf::Options::LoadExternalBuffers |
                              fastgltf::Options::LoadExternalImages |
                              fastgltf::Options::GenerateMeshIndices;

glm::mat4 toGlm(const fastgltf::math::fmat4x4 &m) {
    return glm::make_mat4(m.data());
}

// Reads a VEC2/VEC3/VEC4 attribute into each vertex via setter(index, value); leaves
// defaults untouched if the attribute is absent from this primitive.
template <typename ElementType, typename Setter>
void fillAttribute(const fastgltf::Asset &asset, const fastgltf::Primitive &primitive,
                   std::string_view attributeName, Setter &&setter) {
    const auto *attribute = primitive.findAttribute(attributeName);
    if (attribute == primitive.attributes.end())
        return;
    const auto &accessor = asset.accessors[attribute->accessorIndex];
    if (!accessor.bufferViewIndex.has_value())
        return;
    fastgltf::iterateAccessorWithIndex<ElementType>(
        asset, accessor, [&](ElementType value, std::size_t index) { setter(index, value); });
}

std::optional<Primitive> loadPrimitive(const fastgltf::Asset &asset, const fastgltf::Primitive &gltfPrimitive) {
    if (gltfPrimitive.type != fastgltf::PrimitiveType::Triangles)
        return std::nullopt;

    const auto *positionAttribute = gltfPrimitive.findAttribute("POSITION");
    if (positionAttribute == gltfPrimitive.attributes.end())
        return std::nullopt;
    const auto &positionAccessor = asset.accessors[positionAttribute->accessorIndex];
    if (!positionAccessor.bufferViewIndex.has_value())
        return std::nullopt;

    Primitive primitive;
    primitive.vertices.resize(positionAccessor.count);

    fastgltf::iterateAccessorWithIndex<glm::vec3>(
        asset, positionAccessor, [&](glm::vec3 value, std::size_t index) { primitive.vertices[index].position = value; });
    fillAttribute<glm::vec3>(asset, gltfPrimitive, "NORMAL",
                             [&](std::size_t index, glm::vec3 value) { primitive.vertices[index].normal = value; });
    fillAttribute<glm::vec4>(asset, gltfPrimitive, "TANGENT",
                             [&](std::size_t index, glm::vec4 value) { primitive.vertices[index].tangent = value; });
    fillAttribute<glm::vec2>(asset, gltfPrimitive, "TEXCOORD_0",
                             [&](std::size_t index, glm::vec2 value) { primitive.vertices[index].texCoord = value; });

    if (gltfPrimitive.indicesAccessor.has_value()) {
        const auto &indexAccessor = asset.accessors[gltfPrimitive.indicesAccessor.value()];
        if (indexAccessor.bufferViewIndex.has_value()) {
            primitive.indices.resize(indexAccessor.count);
            fastgltf::copyFromAccessor<std::uint32_t>(asset, indexAccessor, primitive.indices.data());
        }
    }

    if (gltfPrimitive.materialIndex.has_value())
        primitive.materialIndex = gltfPrimitive.materialIndex.value();

    return primitive;
}

// Resolves a glTF TextureInfo (or NormalTextureInfo, which points at a Texture) down to
// the Model::images index it ultimately references. Templated on the whole optional type
// (rather than fastgltf::Optional<TextureInfoT>) because fastgltf::Optional is an alias
// over std::conditional_t and so isn't a deducible context.
template <typename OptionalTextureInfo>
std::optional<std::size_t> resolveImageIndex(const fastgltf::Asset &asset, const OptionalTextureInfo &textureInfo) {
    if (!textureInfo.has_value() || textureInfo->textureIndex >= asset.textures.size())
        return std::nullopt;
    const auto &imageIndex = asset.textures[textureInfo->textureIndex].imageIndex;
    if (!imageIndex.has_value())
        return std::nullopt;
    return imageIndex.value();
}

MaterialData loadMaterial(const fastgltf::Asset &asset, const fastgltf::Material &material) {
    MaterialData result;
    result.baseColorFactor = glm::make_vec4(material.pbrData.baseColorFactor.data());
    result.metallicFactor = material.pbrData.metallicFactor;
    result.roughnessFactor = material.pbrData.roughnessFactor;
    result.alphaCutoff = material.alphaCutoff;
    result.doubleSided = material.doubleSided;
    result.baseColorTextureIndex = resolveImageIndex(asset, material.pbrData.baseColorTexture);
    result.metallicRoughnessTextureIndex = resolveImageIndex(asset, material.pbrData.metallicRoughnessTexture);
    result.normalTextureIndex = resolveImageIndex(asset, material.normalTexture);
    return result;
}

ImageData decodeImage(const fastgltf::Asset &asset, const fastgltf::Image &image, const std::filesystem::path &directory) {
    ImageData result;
    int width = 0, height = 0, channels = 0;
    unsigned char *decoded = nullptr;

    std::visit(fastgltf::visitor{
                   [](const auto &) {},
                   [&](const fastgltf::sources::URI &uri) {
                       const std::filesystem::path imagePath =
                           uri.uri.isLocalPath() ? (directory / uri.uri.fspath()) : std::filesystem::path(uri.uri.path());
                       decoded = stbi_load(imagePath.string().c_str(), &width, &height, &channels, 4);
                   },
                   [&](const fastgltf::sources::Array &array) {
                       decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(array.bytes.data()),
                                                       static_cast<int>(array.bytes.size()), &width, &height, &channels, 4);
                   },
                   [&](const fastgltf::sources::BufferView &bufferViewSource) {
                       const auto &bufferView = asset.bufferViews[bufferViewSource.bufferViewIndex];
                       const auto &buffer = asset.buffers[bufferView.bufferIndex];
                       std::visit(fastgltf::visitor{
                                      [](const auto &) {},
                                      [&](const fastgltf::sources::Array &array) {
                                          decoded = stbi_load_from_memory(
                                              reinterpret_cast<const stbi_uc *>(array.bytes.data() + bufferView.byteOffset),
                                              static_cast<int>(bufferView.byteLength), &width, &height, &channels, 4);
                                      },
                                  },
                                  buffer.data);
                   },
               },
               image.data);

    if (decoded != nullptr) {
        result.width = width;
        result.height = height;
        const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        result.pixels.assign(decoded, decoded + pixelCount);
        stbi_image_free(decoded);
    } else {
        std::cerr << "loadModel: failed to decode image '" << image.name << "'\n";
    }
    return result;
}

// Bakes a node's world matrix into a copy of that mesh's CPU vertex data, so the resulting
// Model needs no scene-graph knowledge to be drawn correctly.
void transformPrimitiveInPlace(Primitive &primitive, const glm::mat4 &worldMatrix) {
    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(worldMatrix));
    for (Vertex &vertex : primitive.vertices) {
        vertex.position = glm::vec3(worldMatrix * glm::vec4(vertex.position, 1.0f));
        vertex.normal = glm::normalize(normalMatrix * vertex.normal);
        const glm::vec3 tangentDir(vertex.tangent);
        if (glm::dot(tangentDir, tangentDir) > 0.0f)
            vertex.tangent = glm::vec4(glm::normalize(normalMatrix * tangentDir), vertex.tangent.w);
    }
}

} // namespace

std::optional<Model> loadModel(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "loadModel: file not found: " << path << "\n";
        return std::nullopt;
    }

    auto gltfFile = fastgltf::MappedGltfFile::FromPath(path);
    if (!bool(gltfFile)) {
        std::cerr << "loadModel: failed to open " << path << ": " << fastgltf::getErrorMessage(gltfFile.error()) << "\n";
        return std::nullopt;
    }

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(gltfFile.get(), path.parent_path(), kGltfOptions);
    if (assetResult.error() != fastgltf::Error::None) {
        std::cerr << "loadModel: failed to parse " << path << ": " << fastgltf::getErrorMessage(assetResult.error()) << "\n";
        return std::nullopt;
    }
    fastgltf::Asset &asset = assetResult.get();

    Model model;

    model.images.reserve(asset.images.size());
    for (auto &image : asset.images)
        model.images.push_back(decodeImage(asset, image, path.parent_path()));

    model.materials.reserve(asset.materials.size());
    for (auto &material : asset.materials)
        model.materials.push_back(loadMaterial(asset, material));

    // Cache each gltf mesh's untransformed primitives once, then stamp out a transformed
    // copy per scene-node instance below (handles the same mesh being referenced by
    // multiple nodes).
    std::vector<std::vector<Primitive>> meshPrimitives(asset.meshes.size());
    for (std::size_t i = 0; i < asset.meshes.size(); ++i) {
        for (auto &gltfPrimitive : asset.meshes[i].primitives) {
            if (auto primitive = loadPrimitive(asset, gltfPrimitive))
                meshPrimitives[i].push_back(std::move(*primitive));
        }
    }

    const std::size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex < asset.scenes.size()) {
        fastgltf::iterateSceneNodes(
            asset, sceneIndex, fastgltf::math::fmat4x4(), [&](fastgltf::Node &node, const fastgltf::math::fmat4x4 &worldMatrix) {
                if (!node.meshIndex.has_value())
                    return;
                const glm::mat4 world = toGlm(worldMatrix);
                MeshData mesh;
                for (const Primitive &sourcePrimitive : meshPrimitives[node.meshIndex.value()]) {
                    Primitive instance = sourcePrimitive;
                    transformPrimitiveInPlace(instance, world);
                    mesh.primitives.push_back(std::move(instance));
                }
                model.meshes.push_back(std::move(mesh));
            });
    }

    return model;
}
