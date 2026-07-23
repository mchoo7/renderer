struct VSInput {
    float2 pos;
    float2 uv;
    float4 col;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv;
    float4 col;
};

struct PushConstants {
    float2 scale;
    float2 translate;
};

[shader("vertex")]
VSOutput main(VSInput input, uniform PushConstants pc) {
    VSOutput output;
    output.uv = input.uv;
    output.col = input.col;
    output.position = float4(input.pos * pc.scale + pc.translate, 0.0, 1.0);
    return output;
}

Sampler2D fontTexture;

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    return input.col * fontTexture.Sample(input.uv);
}
