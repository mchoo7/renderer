#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Euler-angle FPS-style camera. Produces a right-handed view matrix (glm::lookAt), which
// is shared by every backend — only the projection matrix differs per API (see projection.h).

enum class CameraMovement {
    Forward,
    Backward,
    Left,
    Right,
};

class Camera {
  public:
    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;

    float Yaw;
    float Pitch;

    float MovementSpeed = 2.5f;
    float MouseSensitivity = 0.1f;
    float Zoom = 45.0f;

    explicit Camera(glm::vec3 position = glm::vec3(0.0f), glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f),
                    float yaw = -90.0f, float pitch = 0.0f)
        : Position(position), Front(0.0f, 0.0f, -1.0f), WorldUp(up), Yaw(yaw), Pitch(pitch) {
        updateCameraVectors();
    }

    [[nodiscard]] glm::mat4 GetViewMatrix() const {
        return glm::lookAt(Position, Position + Front, Up);
    }

    void ProcessKeyboard(CameraMovement direction, float deltaTime) {
        const float velocity = MovementSpeed * deltaTime;
        switch (direction) {
        case CameraMovement::Forward:
            Position += Front * velocity;
            break;
        case CameraMovement::Backward:
            Position -= Front * velocity;
            break;
        case CameraMovement::Left:
            Position -= Right * velocity;
            break;
        case CameraMovement::Right:
            Position += Right * velocity;
            break;
        }
    }

    void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset;
        Pitch += yoffset;

        if (constrainPitch)
            Pitch = glm::clamp(Pitch, -89.0f, 89.0f);

        updateCameraVectors();
    }

    void ProcessMouseScroll(float yoffset) {
        Zoom = glm::clamp(Zoom - yoffset, 1.0f, 45.0f);
    }

  private:
    void updateCameraVectors() {
        const glm::vec3 front{
            std::cos(glm::radians(Yaw)) * std::cos(glm::radians(Pitch)),
            std::sin(glm::radians(Pitch)),
            std::sin(glm::radians(Yaw)) * std::cos(glm::radians(Pitch)),
        };
        Front = glm::normalize(front);
        Right = glm::normalize(glm::cross(Front, WorldUp));
        Up = glm::normalize(glm::cross(Right, Front));
    }
};
