// Music mode (docs/audio-dsp.md, "Music mode"): the sound as analytical geometry over a dark field, the background the
// controls are drawn over. A ring of 64 spectrum bars (the 32 bands mirrored left and right, lows at the top) around a
// glass orb that breathes with the level, and the waveform envelope as a ribbon across the middle, behind the orb.
// Signed distances with one pixel of antialiasing; no textures, samplers or vertex buffers.
cbuffer Visual : register(b0) {
    float2 size; float clockTime; float level;
    float peak; float scale; float dim; float reserved;
    uint4 bands[2];  // Band b: byte b.
    uint4 wave[10];  // Column c: least sample in byte 2c, greatest in byte 2c + 1, each biased by 128.
};
static const float PI = 3.141592653589793;
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint Word(uint4 v, uint i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }
float Band(uint b) { return ((Word(bands[b >> 4], (b >> 2) & 3) >> ((b & 3) * 8)) & 255) / 235.0; } // 1: 0 dB.
float2 Column(uint c) {
    const uint pair = (Word(wave[c >> 3], (c >> 1) & 3) >> ((c & 1) * 16)) & 0xffff;
    return float2(((float)(pair & 255) - 128) / 127, ((float)(pair >> 8) - 128) / 127);
}
float Cover(float d) { return 1 - smoothstep(-0.75, 0.75, d); } // d in pixels, negative inside.
float4 PS(float4 position : SV_Position) : SV_Target {
    const float2 p = position.xy;
    const float unit = min(size.x, size.y);
    const float2 centre = float2(size.x * 0.5, size.y * 0.5 - 0.04 * unit); // A little above the middle: the dock is below.
    const float2 q = p - centre;
    const float r = length(q);
    // The field: deep indigo at the centre to near black, warmed by the level.
    float3 colour = lerp(float3(0.060, 0.052, 0.120), float3(0.008, 0.010, 0.022), saturate(r / unit * 1.3));
    colour += float3(0.10, 0.05, 0.18) * level * exp(-r * r / (unit * unit) * 5);
    // The waveform ribbon: the envelope between the columns' least and greatest samples, a pixel thick at least, fading
    // in from the window's edges.
    const float across = saturate(p.x / size.x);
    const float t = across * 79;
    const uint c0 = (uint)floor(t), c1 = min(c0 + 1, 79u);
    const float2 e = lerp(Column(c0), Column(c1), t - c0);
    const float amplitude = 0.17 * unit;
    const float v = -(p.y - centre.y);
    const float low = min(e.x, e.y) * amplitude - scale, high = max(e.x, e.y) * amplitude + scale;
    const float ribbon_d = max(low - v, v - high);
    const float edges = smoothstep(0.0, 0.14, across) * smoothstep(0.0, 0.14, 1 - across);
    const float3 ribbon = lerp(float3(0.20, 0.78, 0.95), float3(0.66, 0.36, 1.00), across);
    colour = lerp(colour, ribbon, Cover(ribbon_d) * 0.55 * edges);
    colour += ribbon * exp(-max(ribbon_d, 0) / (7 * scale)) * 0.10 * edges;
    // The orb: glass, lit from within by the level; its rim brightens with the peak.
    const float radius = unit * (0.13 + 0.03 * level);
    const float orb = r - radius;
    const float3 glass = float3(0.10, 0.09, 0.21) + float3(0.24, 0.19, 0.46) * saturate(1 - r / radius) * (0.35 + 0.65 * level);
    colour = lerp(colour, glass, Cover(orb) * 0.88);
    colour += float3(0.72, 0.62, 1.00) * (1 - smoothstep(0, 1.6 * scale, abs(orb))) * (0.30 + 0.55 * peak);
    // The bars: angle from the top, clockwise on the right and mirrored on the left; bar i is band i, a capsule from just
    // outside the orb, as long as its energy.
    const float angle = abs(atan2(q.x, -q.y)); // 0 at the top, PI at the bottom.
    const float cell = PI / 32;
    const float index = min(floor(angle / cell), 31);
    const float energy = saturate(Band((uint)index));
    const float start = radius + 0.014 * unit;
    const float reach = max(0.22 * unit * energy, 1.5 * scale);
    const float offset = angle - (index + 0.5) * cell;
    const float side = r * sin(offset), along = r * cos(offset) - start;
    const float half_width = max(start * cell * 0.30, scale);
    const float bar_d = length(float2(side, along - clamp(along, 0, reach))) - half_width;
    const float3 bar = lerp(float3(0.26, 0.86, 1.00), float3(0.82, 0.42, 1.00), index / 31) * (0.55 + 0.45 * energy);
    colour = lerp(colour, bar, Cover(bar_d));
    colour += bar * exp(-max(bar_d, 0) / (5 * scale)) * 0.20 * energy;
    colour *= 1 - 0.35 * dim; // Paused: the picture holds, dimmed.
    return float4(saturate(colour), 1);
}
