// Test-only: compares a produced RGBA8 image with a reference image; only two scalars are read back
// ([0] sum of absolute channel differences in 8-bit steps, [4] largest single difference).
Texture2D<float4> produced : register(t0);
Texture2D<float4> expected : register(t1);
RWByteAddressBuffer result : register(u0);
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    uint width, height; produced.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    uint3 a = (uint3)round(saturate(produced[id.xy].rgb) * 255.0), b = (uint3)round(saturate(expected[id.xy].rgb) * 255.0);
    uint3 d = uint3(a.r > b.r ? a.r - b.r : b.r - a.r, a.g > b.g ? a.g - b.g : b.g - a.g, a.b > b.b ? a.b - b.b : b.b - a.b);
    result.InterlockedAdd(0, d.r + d.g + d.b);
    result.InterlockedMax(4, max(d.r, max(d.g, d.b)));
}
