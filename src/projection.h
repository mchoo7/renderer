#pragma once

#include "backend.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>

// Projection matrices are the one place OpenGL, Vulkan, Metal and DirectX genuinely
// disagree: camera-space handedness and NDC depth range differ per API. The view matrix
// (Camera::GetViewMatrix in camera.h) is shared by all of them; only this differs.
//
//   OpenGL   - right-handed camera space, NDC depth [-1, 1]
//   Vulkan   - right-handed camera space (same view matrix as GL), NDC depth [0, 1],
//              clip-space Y flipped relative to GL/Metal/DX
//   Metal    - left-handed camera space, NDC depth [0, 1]
//   DirectX  - left-handed camera space, NDC depth [0, 1] (11 and 12 agree)

inline glm::mat4 perspective(Backend backend, float fovYRadians, float aspect, float zNear, float zFar) {
    switch (backend) {
#ifdef APP_BACKEND_OPENGL
    case Backend::OpenGL:
        return glm::perspectiveRH_NO(fovYRadians, aspect, zNear, zFar);
#endif
#ifdef APP_BACKEND_VULKAN
    case Backend::Vulkan: {
        glm::mat4 proj = glm::perspectiveRH_ZO(fovYRadians, aspect, zNear, zFar);
        proj[1][1] *= -1.0f;
        return proj;
    }
#endif
#ifdef APP_BACKEND_DX11
    case Backend::DX11:
        return glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar);
#endif
#ifdef APP_BACKEND_DX12
    case Backend::DX12:
        return glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar);
#endif
#ifdef APP_BACKEND_METAL
    case Backend::Metal:
        return glm::perspectiveLH_ZO(fovYRadians, aspect, zNear, zFar);
#endif
    }

    return glm::mat4(1.0f);
}

inline glm::mat4 orthographic(Backend backend, float left, float right, float bottom, float top, float zNear, float zFar) {
    switch (backend) {
#ifdef APP_BACKEND_OPENGL
    case Backend::OpenGL:
        return glm::orthoRH_NO(left, right, bottom, top, zNear, zFar);
#endif
#ifdef APP_BACKEND_VULKAN
    case Backend::Vulkan: {
        glm::mat4 proj = glm::orthoRH_ZO(left, right, bottom, top, zNear, zFar);
        proj[1][1] *= -1.0f;
        return proj;
    }
#endif
#ifdef APP_BACKEND_DX11
    case Backend::DX11:
        return glm::orthoLH_ZO(left, right, bottom, top, zNear, zFar);
#endif
#ifdef APP_BACKEND_DX12
    case Backend::DX12:
        return glm::orthoLH_ZO(left, right, bottom, top, zNear, zFar);
#endif
#ifdef APP_BACKEND_METAL
    case Backend::Metal:
        return glm::orthoLH_ZO(left, right, bottom, top, zNear, zFar);
#endif
    }

    return glm::mat4(1.0f);
}
