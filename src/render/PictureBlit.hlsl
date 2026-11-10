// Scales a converted picture (the top-left `extent` of an RGB slot or the history texture) into the viewport. Used
// when the picture was converted for a different window size than the one being presented (just after a resize, or
// when recomposing a paused picture), and when it was converted for another zoom region than the one asked for now
// (docs/picture.md, "Live zoom"); matching sizes and regions are copied instead.
Texture2D<float4> picture : register(t0);
SamplerState linearClamp : register(s0);
// extent: picture size / texture size. limit: the centre of the picture's last texel in each axis. Sampling is
// clamped to it: the texture is larger than the picture (an RGB slot sized for the largest window), and beyond the
// picture it holds whatever an earlier, larger picture left, which bilinear filtering at the right and bottom edges
// would otherwise blend in. origin, span: the view, in fractions of the picture (0, 1: all of it); outside the
// picture the view is black.
cbuffer Blit : register(b0) { float2 extent; float2 limit; float2 origin; float2 span; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    Vertex o; o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;
}
float4 PS(Vertex v) : SV_Target {
    float2 q = origin + v.uv * span;
    if (any(q < 0) || any(q > 1)) return float4(0, 0, 0, 1);
    return float4(picture.SampleLevel(linearClamp, min(q * extent, limit), 0).rgb, 1);
}
