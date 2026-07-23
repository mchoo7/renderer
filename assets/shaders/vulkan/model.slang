struct VSInput {
    float3 position;
    float3 normal;
    float2 texCoord;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 normal;
    float2 texCoord;
};

// Vertices are already in world space (loadModel bakes in each node transform), so the only
// matrix needed is the camera view-projection. baseColorFactor/textureIndex are per-primitive
// material data; textureIndex < 0 means the primitive has no albedo texture.
struct ShaderData {
    float4x4 view;
    float4x4 projection;
    float4 baseColorFactor;
    int textureIndex;
}

[shader("vertex")]
VSOutput main(VSInput input, uniform ShaderData *data) {
    VSOutput output;
    output.normal = input.normal;
    output.texCoord = input.texCoord;
    output.position = mul(data.projection, mul(data.view, float4(input.position, 1.0)));
    return output;
}

Sampler2D textures[];

[shader("fragment")]
float4 main(VSOutput input, uniform ShaderData *data) : SV_Target {
    float4 color = data.baseColorFactor;
    if (data.textureIndex >= 0)
        color *= textures[data.textureIndex].Sample(input.texCoord);

    float3 n = normalize(input.normal);
    float3 lightDir = normalize(float3(0.5, 1.0, 0.3));
    float diffuse = max(dot(n, lightDir), 0.0);
    return float4(color.rgb * (0.25 + diffuse * 0.75), color.a);
}
