cmake_minimum_required(VERSION 4.2)
project(renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default options
if(APPLE)
    set(DEFAULT_METAL ON)
else()
    set(DEFAULT_OPENGL ON)
endif()

if(WIN32)
    set(DEFAULT_DX11 ON)
    set(DEFAULT_DX12 ON)
endif()

# Backend options
option(ENABLE_OPENGL "Build with OpenGL 4.6 backend" ${DEFAULT_OPENGL})
option(ENABLE_VULKAN "Build with Vulkan 1.4 backend" ON)
option(ENABLE_DX11   "Build with DirectX 11 backend" ${DEFAULT_DX11})
option(ENABLE_DX12   "Build with DirectX 12 backend" ${DEFAULT_DX12})
option(ENABLE_METAL  "Build with Metal 4 backend"    ${DEFAULT_METAL})

# Platform guards
if((ENABLE_DX11 OR ENABLE_DX12) AND NOT WIN32)
    message(WARNING "DX11/DX12 are Windows-only. Disabling.")
    set(ENABLE_DX11 OFF CACHE BOOL "" FORCE)
    set(ENABLE_DX12 OFF CACHE BOOL "" FORCE)
endif()

if(ENABLE_OPENGL AND APPLE)
    message(WARNING "OpenGL is not supported on macOS. Disabling.")
    set(ENABLE_OPENGL OFF CACHE BOOL "" FORCE)
endif()

if(ENABLE_METAL AND NOT APPLE)
    message(WARNING "Metal is macOS-only. Disabling.")
    set(ENABLE_METAL OFF CACHE BOOL "" FORCE)
endif()

if(NOT ENABLE_OPENGL AND NOT ENABLE_VULKAN AND NOT ENABLE_DX11 AND NOT ENABLE_DX12 AND NOT ENABLE_METAL)
    message(FATAL_ERROR "At least one graphics backend must be enabled.")
endif()

# --- External Dependencies ---

include(FetchContent)

# GLFW & GLM — installed via the system package manager
find_package(glfw3 3.4 REQUIRED)
find_package(glm REQUIRED)

# fastgltf (glTF 2.0 model loading)
set(FASTGLTF_ENABLE_TESTS    OFF CACHE INTERNAL "")
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE INTERNAL "")
set(FASTGLTF_ENABLE_DOCS     OFF CACHE INTERNAL "")
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        v0.9.0
)
FetchContent_MakeAvailable(fastgltf)

# stb (header-only — image loading etc.)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# ImGui (platform layer only — graphical backends are implemented by this project)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        8936b58fe26e8c3da834b8f60b06511d537b4c63
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR "${imgui_SOURCE_DIR}")

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
)

# OpenGL only
if(ENABLE_OPENGL)
    find_package(OpenGL REQUIRED)

    FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/dav1dde/glad
        GIT_TAG        v2.0.8
        SOURCE_SUBDIR  cmake
    )
    FetchContent_MakeAvailable(glad)

    glad_add_library(glad STATIC REPRODUCIBLE API gl:core=4.6 EXTENSIONS NONE)
endif()

# Vulkan helper libraries
if(ENABLE_VULKAN)
    # Vulkan and Slang (shader compilation API) are both bundled with the Vulkan
    # SDK. VULKAN_SDK is set by sourcing the SDK's setup-env.sh; hint CMake at it
    # if present.
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
    endif()
    find_package(Vulkan REQUIRED)

    FetchContent_Declare(vk-bootstrap
        GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
        GIT_TAG        v1.4.341
    )
    FetchContent_MakeAvailable(vk-bootstrap)

    find_package(slang CONFIG REQUIRED)
    set(SLANG_TARGET slang::slang)

    # Work around a packaging bug in this Vulkan SDK's slangTargets.cmake: the
    # path it computes for INTERFACE_INCLUDE_DIRECTORIES is one directory level
    # short (".../lib/include/slang" instead of ".../include/slang").
    # https://vulkan.lunarg.com/issue/view/6a22f8d6c2cf0451740b5e54
    if(LINUX)
        set_target_properties(${SLANG_TARGET} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "$ENV{VULKAN_SDK}/include/slang"
        )
    endif()
endif()

# Metal only
if(ENABLE_METAL)
    FetchContent_Declare(metal_cpp
            GIT_REPOSITORY https://github.com/apple/metal-cpp.git
            GIT_TAG        release/metal-cpp_macOS27_iOS27
    )
    FetchContent_MakeAvailable(metal_cpp)

    add_library(metal_cpp INTERFACE)
    target_include_directories(metal_cpp INTERFACE ${metal_cpp_SOURCE_DIR})
    target_link_libraries(metal_cpp INTERFACE
            "-framework Metal"
            "-framework Foundation"
            "-framework QuartzCore"
    )
endif()

# --- Backend sources (compiled conditionally) ---
set(BACKEND_SOURCES "")
if(ENABLE_OPENGL)
    list(APPEND BACKEND_SOURCES src/backends/opengl_backend.cpp)
endif()
# Add src/backends/<name>_backend.cpp here as other backends are implemented

# --- Executable Target ---
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/model.cpp
    ${IMGUI_SOURCES}
    ${BACKEND_SOURCES}
)

# --- Include Directories ---
target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# --- Link Libraries ---
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    glm::glm
    fastgltf::fastgltf
    stb
)

# Prevent GLFW from including platform GL headers — glad owns that responsibility
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_INCLUDE_NONE)

# Lets src/paths.h resolve bundled assets (e.g. assets/models/*) regardless of the
# working directory the binary is launched from.
target_compile_definitions(${PROJECT_NAME} PRIVATE PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}")

# --- Backend-specific compile definitions and libraries ---
if(ENABLE_OPENGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE glad OpenGL::GL)
    target_compile_definitions(${PROJECT_NAME} PRIVATE APP_BACKEND_OPENGL)
endif()

if(ENABLE_VULKAN)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Vulkan::Vulkan
        vk-bootstrap::vk-bootstrap
        ${SLANG_TARGET}
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE APP_BACKEND_VULKAN)
endif()

if(ENABLE_DX11)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE APP_BACKEND_DX11)
endif()

if(ENABLE_DX12)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE APP_BACKEND_DX12)
endif()

if(ENABLE_METAL)
    target_link_libraries(${PROJECT_NAME} PRIVATE metal_cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE APP_BACKEND_METAL)
endif()

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
