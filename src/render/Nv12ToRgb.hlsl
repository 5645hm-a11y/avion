// Planar 4:2:0 (NV12 8-bit or P010 10-bit) to display RGB, with crop, scaling (Reconstruct.hlsli: bilinear or
// edge-directed, optional contrast-adaptive sharpening), range expansion, the stream's Y'CbCr matrix and, for
// BT.2020 primaries, gamut conversion to the BT.709 display in linear light.
#include "Reconstruct.hlsli"
float3 Convert(float y, float2 c) {
    // One code value in UNORM units: 8-bit samples are v/255; P010 stores the 10-bit value in the high bits.
    float code = tenBit ? 64.0/65535.0 : 1.0/255.0;
    float depth = tenBit ? 4.0 : 1.0; // 8-bit reference levels scale by 2^(bitDepth-8) (BT.2100 Table 9).
    c -= 128.0 * depth * code;
    if (fullRange) { float peak = (tenBit ? 1023.0 : 255.0) * code; y /= peak; c /= peak; }
    else { y = (y - 16.0 * depth * code) / (219.0 * depth * code); c /= 224.0 * depth * code; }
    // matrix_coeffs: 1 = BT.709, 9 = BT.2020 non-constant luminance, otherwise BT.601.
    float kr = matrixId == 1 ? 0.2126 : matrixId == 9 ? 0.2627 : 0.299;
    float kb = matrixId == 1 ? 0.0722 : matrixId == 9 ? 0.0593 : 0.114;
    float red = y + 2*(1-kr)*c.y;
    float blue = y + 2*(1-kb)*c.x;
    float green = (y-kr*red-kb*blue)/(1-kr-kb);
    float3 color = saturate(float3(red, green, blue));
    float3 shown = color;
    [branch] if (HdrOutput()) {
        // API 14, an HDR texture: display light (BT.1886, gamma 2.4) with white at the reference white (BT.2408: target_nits),
        // in BT.2020, as PQ.
        float3 light = pow(color, 2.4) * asfloat(targetBits);
        shown = NitsToPq3(primaries == 9 ? light : Bt709To2020(light));
    } else if (primaries == 9) {
        // BT.2020 -> BT.709 primaries (ITU-R BT.2087) applied to display light (BT.1886, gamma 2.4).
        float3 light = pow(color, 2.4);
        light = float3(dot(float3( 1.660491, -0.587641, -0.072850), light),
                       dot(float3(-0.124550,  1.132900, -0.008349), light),
                       dot(float3(-0.018151, -0.100579,  1.118730), light));
        shown = pow(saturate(light), 1.0 / 2.4);
    }
    return shown;
}
