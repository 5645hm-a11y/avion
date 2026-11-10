// Test-only: measures a converted RGBA8 image of a slanted dark/bright step (analytic edge fixture). One thread per
// output row; only scalars are read back:
//   [0]  largest overshoot beyond [dark, bright], 8-bit steps x 1000 (ringing, halos)
//   [4]  pixels inside the 10%..90% transition band (edge width x rows)
//   [8]  rows with a 50% crossing
//   [12] sum of crossing deviations from the true line, output pixels x 1000 (int, two's complement)
//   [16] sum of squared deviations, output pixels^2 x 1000 (staircase / jaggies)
//   [20] sum of |value - dark|, 8-bit steps x 1000 (flat-field noise when `dark` is the flat level)
Texture2D<float4> image : register(t0);
RWByteAddressBuffer result : register(u0);
cbuffer Metrics : register(b0) {
    float dark, bright;       // Encoded levels of the two sides (or the flat level for noise).
    float sourcePerOutput;    // Scale: source pixels per output pixel.
    float centerX, centerY;   // A point of the true edge, in source pixels.
    float slope;              // dx/dy of the true edge in source pixels.
    uint margin, unused;      // Output rows skipped at the top and bottom.
};
[numthreads(64, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    uint width, height; image.GetDimensions(width, height);
    uint y = id.x;
    if (y < margin || y + margin >= height) return;
    float previous = 0, crossing = 0;
    bool found = false;
    uint band = 0, overshoot = 0, noise = 0;
    for (uint x = 0; x < width; ++x) {
        float v = image[uint2(x, y)].g;
        float t = (v - dark) / (bright - dark);
        overshoot = max(overshoot, (uint)round(max(0.0, max(v - bright, dark - v)) * 255000.0));
        noise += (uint)round(abs(v - dark) * 255000.0);
        if (t > 0.1 && t < 0.9) ++band;
        if (!found && x > 0 && previous < 0.5 && t >= 0.5) { crossing = (x - 1) + (0.5 - previous) / (t - previous); found = true; }
        previous = t;
    }
    result.InterlockedMax(0, overshoot);
    result.InterlockedAdd(4, band);
    result.InterlockedAdd(20, noise);
    if (!found) return;
    // True edge at this output row's centre, in output pixel coordinates (pixel centres at integers).
    float sourceY = (y + 0.5) * sourcePerOutput;
    float trueX = (centerX + (sourceY - centerY) * slope) / sourcePerOutput - 0.5;
    float d = crossing - trueX;
    result.InterlockedAdd(8, 1);
    result.InterlockedAdd(12, (uint)(int)round(d * 1000.0));
    result.InterlockedAdd(16, (uint)round(d * d * 1000.0));
}
