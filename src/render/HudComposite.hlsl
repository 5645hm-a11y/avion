// Every mark is analytical geometry. No textures, samplers, fonts or vertex buffers.
cbuffer Hud : register(b0) {
    float2 center; float scale; float clockTime;
    float energy, freeze, pulse, visibility;
    float progress, scrubOffset, scrubVelocity, hover;
    float denied, coreRadius, dockHalfWidth, dockHalfHeight;
    float2 pointer; float timelineHover, timelineY;
    float elapsedSeconds, durationSeconds, focus, reducedMotion;
    float volume, volumeVisibility, muted, volumeHover;
    float volumeX, volumeHalfWidth, preview; uint loopMarks;
};
// Chapter and scene marks (ui::TimelineMarks): 32 halves of 16 bits, bit 15 a chapter, bits 0-14 the position (1 to
// 32767); 0 ends the list.
cbuffer Marks : register(b1) { uint4 marks[4]; };
// Bookmarks (ui::BookmarkMarks): 32 halves of 16 bits, each a position along the track (1 to 65535); 0 ends the list.
cbuffer Bookmarks : register(b2) { uint4 bookmarks[4]; };
static const float PI = 3.141592653589793;
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
// Monoline clock numerals, analytical capsule segments; tabular spacing never shifts while playing.
float Digit(float2 p, uint digit) {
    const uint masks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    const float2 a[7] = {float2(-2.6,-5),float2(3,-4.5),float2(3,0.5),float2(-2.6,5),float2(-3,0.5),float2(-3,-4.5),float2(-2.6,0)};
    const float2 b[7] = {float2(2.6,-5),float2(3,-0.5),float2(3,4.5),float2(2.6,5),float2(-3,4.5),float2(-3,-0.5),float2(2.6,0)};
    float d = 100;
    [unroll] for (uint i = 0; i < 7; ++i) if ((masks[digit % 10] & (1u << i)) != 0) d = min(d, Segment(p, a[i], b[i]) - 0.6);
    return d;
}
float Clock(float2 p, float seconds) {
    uint s = (uint)clamp(seconds, 0, 359999);
    uint hour = s / 3600, minute = (s / 60) % 60, sec = s % 60;
    float d = 100;
    // H:MM:SS when needed (no leading hour zero), otherwise MM:SS. Minutes and seconds keep their anchor.
    float shift = hour > 0 ? 10 : 0;
    if (hour > 0) {
        d = Digit(p + float2(21,0), hour);
        if (hour >= 10) d = min(d, Digit(p + float2(31,0), hour / 10));
        d = min(d, min(length(p + float2(13,2)), length(p + float2(13,-2))) - 0.7);
    }
    p.x -= shift;
    d = min(d, Digit(p + float2(17,0), minute / 10)); d = min(d, Digit(p + float2(7,0), minute));
    d = min(d, min(length(p + float2(-1,2)), length(p + float2(-1,-2))) - 0.7);
    d = min(d, Digit(p - float2(9,0), sec / 10)); return min(d, Digit(p - float2(19,0), sec));
}
// Exact signed distance to a quadrilateral (Inigo Quilez's polygon distance, winding-number sign).
float Quad(float2 p, float2 v0, float2 v1, float2 v2, float2 v3) {
    float2 v[4] = {v0, v1, v2, v3};
    float d = dot(p - v[0], p - v[0]), s = 1;
    [unroll] for (int i = 0, j = 3; i < 4; j = i, ++i) {
        float2 e = v[j] - v[i], w = p - v[i];
        float2 b = w - e * saturate(dot(w, e) / dot(e, e));
        d = min(d, dot(b, b));
        bool3 c = bool3(p.y >= v[i].y, p.y < v[j].y, e.x * w.y > e.y * w.x);
        if (all(c) || all(!c)) s = -s;
    }
    return s * sqrt(d);
}
// Play/Pause as one morphing shape: two convex quads whose eight vertices travel between the Pause bars
// (t = 0) and the two halves of the Play triangle (t = 1), which meet on x = 2.5 to close into one triangle.
// Design pixels; the triangle is shifted right so its visual centroid sits at the orb centre.
float Glyph(float2 p, float t) {
    t = t * t * (3 - 2 * t);
    const float2 barL0 = float2(-10, -12), barL1 = float2(-3.2, -12), barL2 = float2(-3.2, 12), barL3 = float2(-10, 12);
    const float2 barR0 = float2(3.2, -12), barR1 = float2(10, -12), barR2 = float2(10, 12), barR3 = float2(3.2, 12);
    // Triangle (-9,-13), (14,0), (-9,13); split at x = 2.5 where its edges are at y = +-6.5. The apex keeps a
    // 0.8 px flat so no edge degenerates to zero length.
    const float2 triL0 = float2(-9, -13), triL1 = float2(2.5, -6.5), triL2 = float2(2.5, 6.5), triL3 = float2(-9, 13);
    const float2 triR0 = float2(2.5, -6.5), triR1 = float2(14, -0.4), triR2 = float2(14, 0.4), triR3 = float2(2.5, 6.5);
    float left = Quad(p, lerp(barL0, triL0, t), lerp(barL1, triL1, t), lerp(barL2, triL2, t), lerp(barL3, triL3, t));
    float right = Quad(p, lerp(barR0, triR0, t), lerp(barR1, triR1, t), lerp(barR2, triR2, t), lerp(barR3, triR3, t));
    return min(left, right) - 1.2; // Rounded corners.
}
void Over(inout float4 dst, float3 color, float alpha) {
    alpha = saturate(alpha);
    dst = float4(color * alpha, alpha) + dst * (1 - alpha);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    float2 p = (position.xy - center) / scale;
    float2 extent = float2(dockHalfWidth, dockHalfHeight);
    float glass = RoundBox(p, extent, 29);
    float shadow = RoundBox(p - float2(0, 8), extent - 2, 29);
    float3 white = float3(0.96, 0.975, 1);
    float3 accent = lerp(float3(0.79, 0.88, 0.96), float3(1, 0.57, 0.36), saturate(denied));
    float4 result = 0;
    Over(result, float3(0.005,0.007,0.012), exp(-max(shadow,0) / 9) * 0.36);
    // Stable dark optical density over white/HDR footage; translucency remains visible at the edges.
    float crown = saturate(0.5 - p.y / (2 * dockHalfHeight));
    Over(result, lerp(float3(0.018,0.021,0.029), float3(0.050,0.057,0.072), crown * crown), Cover(glass) * 0.91);
    Over(result, white, Cover(abs(glass + 0.6) - 0.45) * (0.09 + 0.15 * crown));
    Over(result, float3(0.31,0.38,0.50), Cover(glass + 2) * exp(-pow((p.y + dockHalfHeight) / 18, 2)) * 0.10);
    float2 pointerDelta = p - pointer;
    Over(result, accent, exp(-dot(pointerDelta,pointerDelta) / 1600) * hover * Cover(glass + 2) * 0.12);
    float2 button = p + float2(0,12);
    float buttonSdf = length(button) - coreRadius * (1 - pulse * 0.035);
    Over(result, white, Cover(buttonSdf) * (0.05 + 0.07 * hover * exp(-dot(pointer, pointer + float2(0,24)) / 1300)));
    Over(result, white, Cover(abs(buttonSdf) - 0.55) * (focus == 1 ? 0.8 : 0.09));
    Over(result, white, Cover(Glyph(button / 0.65, saturate(freeze))) );
    // Volume pill (KineticHud lays it out): a speaker whose waves follow the level, or an amber cross when muted,
    // and a level track. It fades with volumeVisibility; the fill level and the mute cue arrive already animated.
    if (volumeHalfWidth > 0 && volumeVisibility > 0) {
        float shown = saturate(volumeVisibility), level = saturate(volume), mute = saturate(muted);
        float2 v = p - float2(volumeX, -12);
        float pill = RoundBox(v, float2(volumeHalfWidth, 12), 12);
        Over(result, white, Cover(pill) * (0.055 + 0.035 * volumeHover) * shown);
        Over(result, white, Cover(abs(pill + 0.6) - 0.45) * 0.10 * shown);
        float2 g = v + float2(volumeHalfWidth - 17, 0);
        float speaker = min(RoundBox(g + float2(4.5, 0), float2(1.8, 2.8), 0.6),
                            Quad(g, float2(-3, -2.8), float2(1.5, -6.5), float2(1.5, 6.5), float2(-3, 2.8)) - 0.5);
        Over(result, white, Cover(speaker) * 0.92 * shown);
        float2 ray = g - float2(1.5, 0);
        float reach = length(ray), cone = smoothstep(0.55, 0.75, ray.x / max(reach, 0.001));
        float waves = Cover(abs(reach - 5.5) - 0.65) * smoothstep(0.0, 0.08, level) + Cover(abs(reach - 9.0) - 0.65) * smoothstep(0.45, 0.6, level);
        Over(result, white, waves * cone * (1 - mute) * 0.85 * shown);
        float slash = min(Segment(g, float2(5, -3.5), float2(12, 3.5)), Segment(g, float2(5, 3.5), float2(12, -3.5))) - 0.75;
        Over(result, float3(1, 0.57, 0.36), Cover(slash) * mute * shown);
        float from = 34 - volumeHalfWidth, to = volumeHalfWidth - 12, gauge = lerp(1.5, 2.25, saturate(volumeHover));
        float lane = RoundBox(v - float2((from + to) / 2, 0), float2((to - from) / 2, gauge), gauge);
        float knob = lerp(from, to, level);
        Over(result, white, Cover(lane) * 0.18 * shown);
        Over(result, lerp(accent, float3(0.55, 0.58, 0.64), mute), Cover(lane) * (1 - smoothstep(knob - 0.5, knob + 0.5, v.x)) * lerp(0.95, 0.55, mute) * shown);
        Over(result, white, Cover(length(v - float2(knob, 0)) - (2.6 + 1.2 * saturate(volumeHover))) * lerp(1, 0.6, mute) * shown);
    }
    // A wide click/drag target lives in the model; only the 3-to-6px track is drawn.
    float halfTrack = dockHalfWidth - 28, thickness = lerp(1.5, 3, timelineHover);
    float2 track = p - float2(0,timelineY);
    float trackSdf = RoundBox(track, float2(halfTrack, thickness), thickness);
    Over(result, white, Cover(trackSdf) * (durationSeconds > 0 ? 0.18 : 0.07));
    float head = lerp(-halfTrack, halfTrack, saturate(progress));
    float target = lerp(-halfTrack, halfTrack, saturate(progress + scrubOffset));
    Over(result, accent, Cover(trackSdf) * (1 - smoothstep(head - 0.5, head + 0.5, p.x)) * 0.95);
    // A chapter is a thin tick across the track, a scene a dot above it; both brighten with the track.
    [loop] for (uint m = 0; m < 32; ++m) {
        uint mark = (marks[m / 8][(m / 2) % 4] >> (16 * (m % 2))) & 0xffff;
        if (mark == 0) break;
        float x = lerp(-halfTrack, halfTrack, ((mark & 0x7fff) - 1) / 32766.0);
        if ((mark & 0x8000) != 0) Over(result, white, Cover(Segment(track, float2(x, -thickness - 3.5), float2(x, thickness + 3.5)) - 0.65) * (0.55 + 0.30 * timelineHover));
        else Over(result, white, Cover(length(track - float2(x, -thickness - 4.5)) - 1.15) * (0.35 + 0.30 * timelineHover));
    }
    // The A–B loop (docs/seeking.md, "A–B loop"): a mint bracket at A opening right and one at B opening left, the stretch
    // between them lit; A alone (B not marked yet) is its bracket only.
    uint loopA = loopMarks & 0xffff, loopB = loopMarks >> 16;
    if (loopA != 0) {
        float3 mint = float3(0.47, 0.90, 0.72);
        float reach = thickness + 5, arm = 3.2;
        float ax = lerp(-halfTrack, halfTrack, (loopA - 1) / 65534.0);
        float2 qa = track - float2(ax, 0);
        float bracketA = min(Segment(qa, float2(0, -reach), float2(0, reach)), min(Segment(qa, float2(0, -reach), float2(arm, -reach)), Segment(qa, float2(0, reach), float2(arm, reach)))) - 0.8;
        Over(result, mint, Cover(bracketA) * 0.95);
        if (loopB != 0) {
            float bx = lerp(-halfTrack, halfTrack, (loopB - 1) / 65534.0);
            float2 qb = track - float2(bx, 0);
            float bracketB = min(Segment(qb, float2(0, -reach), float2(0, reach)), min(Segment(qb, float2(0, -reach), float2(-arm, -reach)), Segment(qb, float2(0, reach), float2(-arm, reach)))) - 0.8;
            Over(result, mint, Cover(bracketB) * 0.95);
            float band = RoundBox(track - float2((ax + bx) * 0.5, 0), float2(max((bx - ax) * 0.5, 0.5), thickness + 1.2), thickness + 1.2);
            Over(result, mint, Cover(band) * 0.32);
        }
    }
    // A bookmark is a small amber flag above the track: a staff and a pennant pointing right.
    [loop] for (uint k = 0; k < 32; ++k) {
        uint mark = (bookmarks[k / 8][(k / 2) % 4] >> (16 * (k % 2))) & 0xffff;
        if (mark == 0) break;
        float x = lerp(-halfTrack, halfTrack, (mark - 1) / 65534.0);
        float2 q = track - float2(x, -thickness - 2);
        float staff = Segment(q, float2(0, 0), float2(0, -8)) - 0.6;
        float pennant = Quad(q, float2(0, -8), float2(4.5, -6.7), float2(4.5, -6.3), float2(0, -5));
        Over(result, float3(1, 0.74, 0.30), Cover(min(staff, pennant)) * (0.85 + 0.15 * timelineHover));
    }
    float trail = Segment(track, float2(min(head,target),0), float2(max(head,target) + 0.001,0));
    Over(result, accent, exp(-trail * trail / 24) * saturate(abs(scrubOffset) * 100) * 0.18);
    Over(result, white, Cover(trail - thickness) * saturate(abs(scrubOffset) * 100) * 0.50);
    float tip = length(track - float2(target,0));
    Over(result, accent, exp(-tip * tip / (28 + abs(scrubVelocity) * 50)) * (0.12 + 0.15 * timelineHover));
    // The knob; while the picture is a keyframe preview it is a ring, and an amber diamond marks where that picture is.
    float knobRadius = 2.6 + 1.4 * timelineHover, previewing = saturate(preview);
    Over(result, white, lerp(Cover(tip - knobRadius), Cover(abs(tip - knobRadius - 0.6) - 0.8), previewing) * step(0.001, durationSeconds));
    float2 previewAt = track - float2(head, 0);
    Over(result, float3(1, 0.80, 0.42), Cover(abs(previewAt.x) + abs(previewAt.y) - 3.6) * previewing * step(0.001, durationSeconds));
    if (focus == 2) Over(result, accent, Cover(abs(trackSdf - 5) - 0.5) * 0.7);
    float clockX = dockHalfWidth - 66;
    Over(result, white, Cover(Clock(p - float2(-clockX,-12), elapsedSeconds)) * 0.90);
    Over(result, float3(0.70,0.75,0.83), Cover(Clock(p - float2(clockX,-12), durationSeconds)) * 0.95);
    return result * saturate(visibility);
}
