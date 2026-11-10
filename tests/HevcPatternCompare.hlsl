// Test-only verification pass: compares a decoded luma plane with the analytic source pattern and
// accumulates the absolute error into one counter. Only that counter is ever read back.
Texture2D<float> luma : register(t0);
RWByteAddressBuffer result : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, frame, offset;
    uint tenBit, pad0, pad1, pad2;
};
// Must match Pattern() in hevc_decode_tests.cpp: a diagonal ramp moving 13 samples per frame plus a moving box.
uint Pattern(uint x, uint y, uint f) {
    uint value = 40 + ((x + 2 * y + 13 * f) & 127);
    uint bx = (5 * f) % (width - 32), by = (3 * f) % (height - 32);
    if (x - bx < 32 && y - by < 32) value += 60;
    return value;
}
[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    // P010 stores the 10-bit sample in the upper bits; an 8-bit pattern value v is encoded as v << 8.
    float scale = tenBit ? 65535.0 / 256.0 : 255.0;
    uint decoded = (uint)round(luma.Load(int3(id.xy, 0)) * scale);
    uint expected = Pattern(id.x, id.y, frame);
    result.InterlockedAdd(offset, decoded > expected ? decoded - expected : expected - decoded);
}
