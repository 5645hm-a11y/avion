struct OutlineRange { uint first, count, winding, cell; }; // One glyph to generate into atlas cell `cell`.
StructuredBuffer<float4> edges : register(t0);
StructuredBuffer<OutlineRange> ranges : register(t1);
RWTexture2D<float> atlasOut : register(u0);
cbuffer Build : register(b0) { uint columns; uint cell; uint unused0; uint unused1; };
[numthreads(8,8,1)]
void BuildAtlas(uint3 id : SV_DispatchThreadID) {
    if (id.x >= cell || id.y >= cell) return;
    OutlineRange g = ranges[id.z]; float2 p = id.xy + 0.5;
    float distance2 = 64; int winding = 0;
    [loop] for (uint i = 0; i < g.count; ++i) {
        float4 e = edges[g.first + i]; float2 v = e.zw - e.xy, q = p - e.xy;
        float2 delta = q - v * saturate(dot(q,v) / max(dot(v,v), 1e-12));
        distance2 = min(distance2, dot(delta,delta));
        float cross = v.x * q.y - v.y * q.x;
        if (e.y <= p.y && e.w > p.y && cross > 0) ++winding;
        if (e.y > p.y && e.w <= p.y && cross < 0) --winding;
    }
    bool inside = g.winding != 0 ? winding != 0 : (abs(winding) & 1) != 0;
    atlasOut[uint2((g.cell % columns) * cell, (g.cell / columns) * cell) + id.xy] = sqrt(distance2) * (inside ? -1 : 1);
}
