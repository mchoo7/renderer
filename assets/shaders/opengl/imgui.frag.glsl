#version 460 core

in  vec2 vUV;
in  vec4 vColor;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D uTex;

void main() {
    fragColor = vColor * texture(uTex, vUV);
}