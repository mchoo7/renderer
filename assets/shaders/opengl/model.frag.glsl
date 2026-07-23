#version 460 core

in  vec3 vNormal;
in  vec2 vTexCoord;
out vec4 fragColor;

layout(location = 2) uniform vec4 uBaseColorFactor;
layout(location = 3) uniform int uHasTexture;

layout(binding = 0) uniform sampler2D uAlbedo;

void main() {
    vec4 color = uBaseColorFactor;
    if (uHasTexture != 0)
        color *= texture(uAlbedo, vTexCoord);

    vec3 n = normalize(vNormal);
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diffuse = max(dot(n, lightDir), 0.0);
    fragColor = vec4(color.rgb * (0.25 + diffuse * 0.75), color.a);
}