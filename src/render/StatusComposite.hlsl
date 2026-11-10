// The HUD's status pill. Glass, rim and icon are analytical geometry; the text is one byte of coverage per pixel,
// rasterised by DirectWrite at the pill's exact pixel size and read here one to one (never filtered).
cbuffer Status : register(b0) {
    float2 center; float scale; float visibility;
    float halfWidth, halfHeight, icon, tone;
    float travel, font, pulse, reserved1;
    float reserved2, reserved3, reserved4, reserved5;
    // Set by the pipeline, whole pixels: where this composite's raster lies, its bytes per row (0: no text), the slide.
    float2 textOrigin; float2 textSize;
    float stride; float travelY; float2 unused;
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
void Over(inout float4 dst, float3 color, float alpha) {
    alpha = saturate(alpha);
    dst = float4(color * alpha, alpha) + dst * (1 - alpha);
}
// The icons, 16 design px across, centred on the origin.
float Speaker(float2 g) {
    g.x += 2;
    float body = RoundBox(g + float2(4, 0), float2(1.8, 2.6), 0.5);
    // The cone: a trapezoid widening to the right (its slanted edges as normalised half-planes).
    float slant = (abs(g.y) - (2.6 + 1.3 * (g.x + 2.2))) / 1.64;
    float cone = max(slant, max(-(g.x + 2.2), g.x - 1.5));
    float2 ray = g - float2(1.5, 0); float reach = length(ray);
    float waves = max(min(abs(reach - 4.0) - 0.75, abs(reach - 7.0) - 0.75), 0.6 * reach - ray.x); // Only to the right.
    return min(min(body, cone), waves);
}
float Captions(float2 g) {
    float frame = abs(RoundBox(g, float2(7.5, 5.5), 2)) - 0.8;
    float lines = min(Segment(g, float2(-4.5, -1.2), float2(1.5, -1.2)), Segment(g, float2(-4.5, 2), float2(4.5, 2))) - 0.8;
    return min(frame, lines);
}
float Microphone(float2 g) {
    float capsule = RoundBox(g - float2(0, -2.5), float2(2.4, 4.5), 2.4);
    float cup = max(abs(length(g - float2(0, -1)) - 5) - 0.75, -(g.y + 0.5));
    float stand = min(Segment(g, float2(0, 4), float2(0, 7)), Segment(g, float2(-3, 7), float2(3, 7))) - 0.75;
    return min(capsule, min(cup, stand));
}
float Device(float2 g) { // A headphone band and two cups.
    float band = max(abs(length(g - float2(0, 1)) - 6.2) - 0.8, g.y - 1);
    float cups = min(RoundBox(g - float2(-5.6, 3), float2(1.6, 3), 1), RoundBox(g - float2(5.6, 3), float2(1.6, 3), 1));
    return min(band, cups);
}
float Warning(float2 g) {
    float2 a = float2(0, -7), b = float2(7.5, 6), c = float2(-7.5, 6);
    float edge = min(Segment(g, a, b), min(Segment(g, b, c), Segment(g, c, a))) - 0.85;
    float mark = min(Segment(g, float2(0, -2.5), float2(0, 1.5)) - 0.9, length(g - float2(0, 3.8)) - 1.0);
    return min(edge, mark);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    float2 p = (position.xy - center - float2(0, travelY)) / scale;
    float2 extent = float2(halfWidth, halfHeight);
    // A pill, or a panel of several lines (StatusCue::kLineHeight each) with the pill's corners.
    float radius = min(halfHeight, 19);
    float pill = RoundBox(p, extent, radius);
    float shadow = RoundBox(p - float2(0, 6), extent - 2, radius);
    float3 white = float3(0.96, 0.975, 1);
    float3 amber = float3(1, 0.57, 0.36), cool = float3(0.55, 0.86, 1.0);
    float warning = saturate(1 - abs(tone - 1)), listening = saturate(tone - 1); // Tones 0 info, 1 warning, 2 listening.
    float3 accent = lerp(lerp(float3(0.79, 0.88, 0.96), amber, warning), cool, listening);
    float4 result = 0;
    Over(result, float3(0.005, 0.007, 0.012), exp(-max(shadow, 0) / 8) * 0.34);
    // Listening: a soft halo around the pill, breathing with `pulse`.
    Over(result, cool, exp(-max(pill, 0) / 6) * step(0, pill) * listening * (0.10 + 0.22 * pulse));
    // The dock's glass: a stable dark optical density over any picture, lighter towards its crown.
    float crown = saturate(0.5 - p.y / (2 * halfHeight));
    Over(result, lerp(float3(0.018, 0.021, 0.029), float3(0.052, 0.059, 0.075), crown * crown), Cover(pill) * 0.90);
    Over(result, white, Cover(abs(pill + 0.6) - 0.45) * (0.10 + 0.14 * crown));
    Over(result, amber, Cover(abs(pill + 0.6) - 0.7) * 0.45 * warning);
    Over(result, cool, Cover(abs(pill + 0.6) - 0.7) * (0.25 + 0.45 * pulse) * listening);
    Over(result, float3(0.31, 0.38, 0.50), Cover(pill + 2) * exp(-pow((p.y + halfHeight) / 12, 2)) * 0.10);
    // The icon, before the text (of a panel, its first line).
    float2 g = p - float2(16 + 15 - halfWidth, 19 - halfHeight);
    uint which = (uint)round(icon);
    float mark = which == 1 ? Speaker(g) : which == 2 ? Captions(g) : which == 3 ? Microphone(g) : which == 4 ? Device(g) : which == 5 ? Warning(g) : 100;
    Over(result, which == 5 ? amber : accent, Cover(mark) * 0.95);
    // The text.
    float2 pixel = floor(position.xy) - textOrigin - float2(0, travelY);
    bool inLane = pixel.x >= 0 && pixel.y >= 0 && pixel.x < textSize.x && pixel.y < textSize.y && pill < 0;
    if (stride > 0 && inLane) {
        uint address = (uint)pixel.y * (uint)stride + (uint)pixel.x;
        uint word = coverage.Load(address & ~3u);
        float ink = ((word >> ((address & 3u) * 8u)) & 255u) / 255.0;
        Over(result, white, ink * 0.97);
    }
    return result * saturate(visibility);
}
