struct Glyph { float4 rectangle; float4 uv; };
StructuredBuffer<Glyph> glyphs : register(t1);
Texture2D<float> atlas : register(t0);
// Glyph rectangles are in layout pixels; `scale` and `offset` map them into the window. Outline width and shadow
// offset are window pixels and follow the font size.
cbuffer Draw : register(b0) {
    float2 viewport; float2 offset; uint firstGlyph; float scale; float2 atlasSize; float outlinePx; float shadowPx; float2 reserved;
};
// `local`: texel position inside the glyph's own atlas cell; `cell`: the cell's origin and size in texels. Filtering is
// done here from `local` alone and never leaves the cell, so a glyph renders bit-identically whichever cell it was
// paged into (neighbours never bleed in, and the weights do not depend on where the cell is).
struct Vertex { float4 position : SV_Position; float2 local : TEXCOORD0; float pixelsPerTexel : TEXCOORD1; nointerpolation float4 cell : TEXCOORD2; };
Vertex VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    Glyph g = glyphs[firstGlyph + instance]; float2 q = corners[vertex];
    float2 p = offset + (g.rectangle.xy + q * g.rectangle.zw) * scale;
    Vertex o; o.position = float4(p / viewport * float2(2,-2) + float2(-1,1),0,1);
    o.local = q * g.uv.zw; o.pixelsPerTexel = g.rectangle.z * scale / g.uv.z; o.cell = g.uv; return o;
}
// Bilinear distance at texel position `local` of the cell (texel centres at +0.5), clamped to the cell's texels.
float Distance(float2 local, float4 cell) {
    float2 p = clamp(local, 0.5, cell.zw - 0.5) - 0.5, f = frac(p);
    int2 i = int2(p - f) + int2(cell.xy), j = min(i + 1, int2(cell.xy + cell.zw) - 1);
    float a = atlas.Load(int3(i, 0)), b = atlas.Load(int3(j.x, i.y, 0)), c = atlas.Load(int3(i.x, j.y, 0)), d = atlas.Load(int3(j, 0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float4 PS(Vertex v) : SV_Target {
    float d = Distance(v.local, v.cell) * v.pixelsPerTexel;
    float aa = max(fwidth(d) * 0.6, 0.6);
    float fill = 1 - smoothstep(-aa, aa, d);
    float outline = 1 - smoothstep(outlinePx - aa, outlinePx + aa, d);
    float shadowD = Distance(v.local - float2(0, shadowPx / v.pixelsPerTexel), v.cell) * v.pixelsPerTexel;
    float shadow = (1 - smoothstep(-0.3 * shadowPx, 1.9 * shadowPx, shadowD)) * 0.58;
    float alpha = outline + shadow * (1 - outline);
    float3 color = float3(0.008,0.009,0.013) * alpha;
    color = float3(0.965,0.975,0.99) * fill + color * (1 - fill);
    return float4(color, fill + alpha * (1 - fill));
}
