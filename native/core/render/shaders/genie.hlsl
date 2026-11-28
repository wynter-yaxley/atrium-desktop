// Continuous Genie mesh shader: premultiplied snapshot texture with fwidth outer-edge AA and icon-line clip.

cbuffer GenieParams : register(b0)
{
    float Opacity;
    float CutoutCount;
    float2 Pad;
    float4 Cutouts[4];
};

Texture2D Pic : register(t0);
SamplerState Aniso : register(s0);

struct VSIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float2 edgeUv : TEXCOORD1;
    float clipSignedPx : TEXCOORD2;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 edgeUv : TEXCOORD1;
    float clipDist : SV_ClipDistance0;
};

VSOut VS(VSIn input)
{
    VSOut o;
    o.pos = float4(input.pos, 0.0, 1.0);
    o.uv = input.uv;
    o.edgeUv = input.edgeUv;
    o.clipDist = input.clipSignedPx;
    return o;
}

float4 PS(VSOut input) : SV_Target
{
    float4 col = Pic.Sample(Aniso, input.uv);
    float2 d = min(input.edgeUv, 1.0 - input.edgeUv);
    float2 fw = max(fwidth(input.edgeUv), 1e-5);
    float2 edgeCov = saturate(d / fw + 0.5);
    float edgeAa = min(edgeCov.x, edgeCov.y);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (i < (int)CutoutCount && input.pos.x >= Cutouts[i].x && input.pos.x < Cutouts[i].z && input.pos.y >= Cutouts[i].y && input.pos.y < Cutouts[i].w)
            discard;
    }
    return col * saturate(Opacity * edgeAa);
}
