// The command bar. The bar, icons, counter and caret are analytical geometry; the typed text is one byte of coverage
// per pixel, rasterised by DirectWrite at the bar's exact pixel size and read here one to one (never filtered).
cbuffer Palette : register(b0) {
    float2 center; float scale; float visibility;
    float halfWidth, halfHeight, denied, caret;
    float hint, matches, textLeft, textRight;
    float shake, font, reserved0, reserved1;
    // Set by the pipeline, whole pixels: the text lane's top-left corner and size, the caret's column inside it, the
    // raster's bytes per row (0: no raster yet), and the bar's travel while it fades or shakes.
    float2 textOrigin; float2 textSize;
    float caretX; float stride; float2 travel;
};
ByteAddressBuffer coverage : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float Cover(float distance) { return 1 - smoothstep(-0.8 / scale, 0.8 / scale, distance); }
float Segment(float2 p, float2 a, float2 b) {
    float2 q = p - a, v = b - a;
    return length(q - v * saturate(dot(q, v) / dot(v, v)));
}
float RoundBox(float2 p, float2 halfSize, float r) {
    float2 q = abs(p) - halfSize + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
// The dock's monoline numerals.
float Digit(float2 p, uint digit) {
    const uint masks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    const float2 a[7] = {float2(-2.6,-5),float2(3,-4.5),float2(3,0.5),float2(-2.6,5),float2(-3,0.5),float2(-3,-4.5),float2(-2.6,0)};
    const float2 b[7] = {float2(2.6,-5),float2(3,-0.5),float2(3,4.5),float2(2.6,5),float2(-3,4.5),float2(-3,-0.5),float2(2.6,0)};
    float d = 100;
    [unroll] for (uint i = 0; i < 7; ++i) if ((masks[digit % 10] & (1u << i)) != 0) d = min(d, Segment(p, a[i], b[i]) - 0.6);
    return d;
}
void Over(inout float4 dst, float3 color, float alpha) {
    alpha = saturate(alpha);
    dst = float4(color * alpha, alpha) + dst * (1 - alpha);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    float2 p = (position.xy - center - travel) / scale;
    float2 extent = float2(halfWidth, halfHeight);
    float bar = RoundBox(p, extent, 18);
    float shadow = RoundBox(p - float2(0, 10), extent - 2, 18);
    float refused = saturate(denied);
    bool isCommand = hint == 1, isSearch = hint == 2, isMiss = hint == 3;
    float3 white = float3(0.96, 0.975, 1);
    float3 amber = float3(1, 0.57, 0.36);
    float3 accent = lerp(float3(0.79, 0.88, 0.96), amber, refused);
    float4 result = 0;
    Over(result, float3(0.005, 0.007, 0.012), exp(-max(shadow, 0) / 12) * 0.42);
    // The dock's glass: a stable dark optical density over any footage, lighter towards its crown.
    float crown = saturate(0.5 - p.y / (2 * halfHeight));
    Over(result, lerp(float3(0.018, 0.021, 0.029), float3(0.050, 0.057, 0.072), crown * crown), Cover(bar) * 0.93);
    Over(result, white, Cover(abs(bar + 0.6) - 0.45) * (0.10 + 0.16 * crown));
    Over(result, accent, Cover(abs(bar + 0.6) - 0.7) * max(0.55 * refused, isCommand ? 0.22 : 0));
    Over(result, float3(0.31, 0.38, 0.50), Cover(bar + 2) * exp(-pow((p.y + halfHeight) / 16, 2)) * 0.10);
    // Left: a magnifier while the text is a quote to look for, a chevron once it reads as a command.
    float2 g = p - float2(28 - halfWidth, 0);
    float magnifier = min(abs(length(g + float2(1.5, 1.5)) - 6.2) - 0.9, Segment(g, float2(3.2, 3.2), float2(8, 8)) - 1.0);
    float chevron = min(Segment(g, float2(-3, -6.5), float2(3.5, 0)), Segment(g, float2(3.5, 0), float2(-3, 6.5))) - 1.0;
    Over(result, isCommand ? accent : white, Cover(isCommand ? chevron : magnifier) * (isCommand ? 0.95 : isSearch ? 0.85 : 0.5));
    // The text, then the caret at its end.
    float2 pixel = floor(position.xy) - textOrigin - travel;
    bool inLane = pixel.x >= 0 && pixel.y >= 0 && pixel.x < textSize.x && pixel.y < textSize.y;
    if (stride > 0 && inLane) {
        uint address = (uint)pixel.y * (uint)stride + (uint)pixel.x;
        uint word = coverage.Load(address & ~3u);
        float ink = ((word >> ((address & 3u) * 8u)) & 255u) / 255.0;
        Over(result, white, ink);
    }
    float caretWidth = max(1, round(1.5 * scale));
    bool inCaret = inLane && pixel.x >= caretX && pixel.x < caretX + caretWidth && abs(pixel.y + 0.5 - textSize.y * 0.5) <= font * scale * 0.62;
    Over(result, accent, inCaret ? saturate(caret) : 0);
    // Right: how many cues a search would find, or the Return arrow of a command.
    float2 r = p - float2(halfWidth - 34, 0);
    if (isSearch || isMiss) {
        uint n = (uint)clamp(matches, 0, 999);
        float d = Digit(r - float2(10, 0), n % 10);
        if (n >= 10) d = min(d, Digit(r, (n / 10) % 10));
        if (n >= 100) d = min(d, Digit(r + float2(10, 0), n / 100));
        Over(result, isMiss ? amber : white, Cover(d) * (isMiss ? 0.75 : 0.62));
    }
    if (isCommand) {
        float arrow = min(min(Segment(r, float2(8, -5), float2(8, 2)), Segment(r, float2(8, 2), float2(-4, 2))),
                          min(Segment(r, float2(-4, 2), float2(-0.5, -1.5)), Segment(r, float2(-4, 2), float2(-0.5, 5.5)))) - 0.8;
        Over(result, accent, Cover(arrow) * 0.85);
    }
    return result * saturate(visibility);
}
