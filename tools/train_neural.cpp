// Offline trainer for the micro-ESPCN luma upscaler (media::video::NeuralModel). Not part of playback: it allocates
// freely and uses threads. Deterministic: fixed seed, fixed thread count, fixed-order gradient reduction, and the
// Windows software H.264 codec (Microsoft H.264 Video Encoder / Decoder MFTs) for the compression round trips.
//
// Data: natural photographs shipped with Windows (C:\Windows\Web, decoded with WIC; the 4K folder is skipped because it
// holds larger copies of the same pictures), plus procedural anti-aliased edges, lines and text-like strokes. Every
// fourth photo (sorted by path) is held out for validation. Images are box-reduced below 2000 px.
// Domain: the shader sees 8-bit limited-range luma as UNORM (code/255, codes 16-235), so training uses the same:
// targets v = (16 + 219 Y) / 255 unquantised, inputs = Keys bicubic (a = -0.5) 2x decimation, quantised to 8 bits.
// Compression: each low-resolution picture is also coded as an H.264 intra picture at QP 22, 26 and 30 (grey chroma) and
// decoded back, so the network sees the blocking and ringing of streamed video. A quarter of the patches are clean, the
// rest spread evenly over the three QPs; the target is always the clean high-resolution picture, so super-resolution,
// deblocking and deringing are learnt together. The inference-time deringing clamp (NeuralUpscaleReference) is part of
// the model and is unchanged.
// Output: include/media/render/NeuralWeights.hpp with the weights and the validation report.
//
// Usage: media_train_neural <repository root> [steps]   (steps 0: evaluate the committed weights, write nothing)
#include "media/video/NeuralUpscale.hpp"
#include "media/render/NeuralWeights.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <strmif.h>
#include <codecapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
using media::video::NeuralModel;
namespace fs = std::filesystem;
namespace {
struct Image { std::vector<float> v; int w = 0, h = 0; std::string name; float At(int x, int y) const { return v[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; } };
float ToUnorm(float y) { return (16.0f + 219.0f * y) / 255.0f; }
float FromUnorm(float v) { return (v * 255.0f - 16.0f) / 219.0f; }
std::uint64_t rng = 0x9e3779b97f4a7c15ull;
std::uint32_t Next() { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return static_cast<std::uint32_t>(rng >> 33); }
float Uniform() { return (Next() & 0xffffff) / 16777216.0f; }
float Normal() { const float u = (std::max)(Uniform(), 1e-7f), v = Uniform(); return std::sqrt(-2 * std::log(u)) * std::cos(6.2831853f * v); }
template <class T> void Drop(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Load(IWICImagingFactory* wic, const fs::path& path, Image& image) {
    IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr; IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0; std::vector<std::uint8_t> bgr;
    if (ok) { ok = SUCCEEDED(converter->GetSize(&w, &h)) && w >= 256 && h >= 256; }
    if (ok) { bgr.resize(static_cast<std::size_t>(w) * h * 3); ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 3, static_cast<UINT>(bgr.size()), bgr.data())); }
    Drop(converter); Drop(frame); Drop(decoder);
    if (!ok) return false;
    image.w = static_cast<int>(w); image.h = static_cast<int>(h); image.v.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < image.v.size(); ++i) // BT.709 luma of the sRGB-encoded values, full range [0, 1].
        image.v[i] = (0.0722f * bgr[i * 3] + 0.7152f * bgr[i * 3 + 1] + 0.2126f * bgr[i * 3 + 2]) / 255.0f;
    while ((std::max)(image.w, image.h) > 2000) { // 2x box reduction keeps pixel-level detail comparable to video.
        Image half; half.w = image.w / 2; half.h = image.h / 2; half.v.resize(static_cast<std::size_t>(half.w) * half.h);
        for (int y = 0; y < half.h; ++y) for (int x = 0; x < half.w; ++x)
            half.v[static_cast<std::size_t>(y) * half.w + x] = 0.25f * (image.At(2 * x, 2 * y) + image.At(2 * x + 1, 2 * y) + image.At(2 * x, 2 * y + 1) + image.At(2 * x + 1, 2 * y + 1));
        half.name = image.name; image = std::move(half);
    }
    for (auto& value : image.v) value = ToUnorm(value);
    image.name = path.filename().string();
    return true;
}
// Procedural 512x512: anti-aliased (4x4 supersampled) polygons, strokes and text-like bars over a gradient.
Image Procedural(unsigned seed) {
    Image image; image.w = image.h = 512; image.v.assign(512 * 512, 0); image.name = "procedural " + std::to_string(seed);
    const float base = Uniform(), tilt = Uniform() - 0.5f;
    struct Shape { int kind; float a, b, c, d, value; };
    std::vector<Shape> shapes;
    for (int i = 0; i < 40; ++i) shapes.push_back({static_cast<int>(Next() % 3), Uniform() * 512, Uniform() * 512, Uniform() * 3.1416f, 1.5f + Uniform() * 24, Uniform()});
    for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
        float sum = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
            float value = std::clamp(base + tilt * (px - 256) / 512, 0.0f, 1.0f);
            for (const auto& s : shapes) {
                const float cx = px - s.a, cy = py - s.b, u = cx * std::cos(s.c) + cy * std::sin(s.c), v = -cx * std::sin(s.c) + cy * std::cos(s.c);
                bool inside = false;
                if (s.kind == 0) inside = u > 0 && u < s.d * 4 && v > 0 && v < s.d * 3;            // Rotated rectangle.
                else if (s.kind == 1) inside = std::fabs(v) < s.d * 0.12f + 0.4f && std::fabs(u) < 120; // Thin stroke.
                else inside = std::fabs(v) < s.d * 0.5f && std::fmod(std::fabs(u), s.d * 1.6f) < s.d * 0.8f && std::fabs(u) < 90; // Glyph-like bars.
                if (inside) value = s.value;
            }
            sum += value;
        }
        image.v[static_cast<std::size_t>(y) * 512 + x] = ToUnorm(sum / 16);
    }
    return image;
}
constexpr float kDecimate[8]{-0.01171875f, -0.03515625f, 0.11328125f, 0.43359375f, 0.43359375f, 0.11328125f, -0.03515625f, -0.01171875f};
// Keys bicubic 2x decimation: LR (i, j) centred on HR (2i + 0.5, 2j + 0.5), as an 8-bit code.
std::uint8_t Decimated(const Image& image, int i, int j) {
    float sum = 0;
    for (int b = 0; b < 8; ++b) {
        float row = 0; for (int a = 0; a < 8; ++a) row += kDecimate[a] * image.At(2 * i + a - 3, 2 * j + b - 3);
        sum += kDecimate[b] * row;
    }
    return static_cast<std::uint8_t>(std::lround(std::clamp(sum, 0.0f, 1.0f) * 255.0f));
}

// ---- H.264 round trip through the Windows software codec (intra picture, fixed QP, 4:2:0 with neutral chroma). ----
IMFMediaType* VideoType(const GUID& subtype, int w, int h) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30, 1); MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return t;
}
IMFSample* NewSample(DWORD bytes) {
    IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(bytes, &buffer)) || FAILED(sample->AddBuffer(buffer))) { Drop(sample); Drop(buffer); return nullptr; }
    buffer->Release();
    return sample;
}
bool SelectNv12(IMFTransform* mft) {
    for (DWORD i = 0;; ++i) {
        IMFMediaType* type = nullptr;
        if (FAILED(mft->GetOutputAvailableType(0, i, &type))) return false;
        GUID subtype{}; type->GetGUID(MF_MT_SUBTYPE, &subtype);
        const bool ok = subtype == MFVideoFormat_NV12 && SUCCEEDED(mft->SetOutputType(0, type, 0));
        type->Release();
        if (ok) return true;
    }
}
// Collects every pending output of a synchronous MFT: the bitstream (encoder) or the luma plane (decoder, w x h).
bool Pull(IMFTransform* mft, bool decoder, int w, int h, std::vector<std::uint8_t>& out) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;
        const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        IMFSample* sample = provides ? nullptr : NewSample((std::max)(info.cbSize, static_cast<DWORD>(w * h * 2)));
        if (!provides && !sample) return false;
        MFT_OUTPUT_DATA_BUFFER buffer{0, sample, 0, nullptr}; DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &buffer, &status);
        Drop(buffer.pEvents);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { Drop(sample); return true; }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { Drop(sample); if (!decoder || !SelectNv12(mft)) return false; continue; }
        if (FAILED(hr) || !buffer.pSample) { Drop(sample); return false; }
        IMFMediaBuffer* media = nullptr; bool ok = SUCCEEDED(buffer.pSample->GetBufferByIndex(0, &media));
        IMF2DBuffer* plane = nullptr;
        if (ok && decoder && SUCCEEDED(media->QueryInterface(IID_PPV_ARGS(&plane)))) {
            BYTE* data = nullptr; LONG pitch = 0;
            ok = SUCCEEDED(plane->Lock2D(&data, &pitch));
            if (ok) { out.resize(static_cast<std::size_t>(w) * h); for (int y = 0; y < h; ++y) std::memcpy(out.data() + static_cast<std::size_t>(y) * w, data + static_cast<std::ptrdiff_t>(y) * pitch, static_cast<std::size_t>(w)); plane->Unlock2D(); }
        } else if (ok) {
            BYTE* data = nullptr; DWORD length = 0;
            ok = SUCCEEDED(media->Lock(&data, nullptr, &length));
            if (ok && decoder) { ok = length >= static_cast<DWORD>(w * h); if (ok) out.assign(data, data + static_cast<std::size_t>(w) * h); } // Packed NV12: pitch = width.
            else if (ok) out.insert(out.end(), data, data + length);
            media->Unlock();
        }
        Drop(plane); Drop(media); buffer.pSample->Release();
        if (!ok) return false;
    }
}
bool Feed(IMFTransform* mft, const std::uint8_t* data, std::size_t size, bool decoder, int w, int h, std::vector<std::uint8_t>& out) {
    IMFSample* sample = NewSample(static_cast<DWORD>(size));
    if (!sample) return false;
    IMFMediaBuffer* buffer = nullptr; BYTE* target = nullptr;
    bool ok = SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->Lock(&target, nullptr, nullptr));
    if (ok) { std::memcpy(target, data, size); buffer->Unlock(); buffer->SetCurrentLength(static_cast<DWORD>(size)); }
    Drop(buffer);
    sample->SetSampleTime(0); sample->SetSampleDuration(333333);
    ok = ok && SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) && SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)) &&
         SUCCEEDED(mft->ProcessInput(0, sample, 0)) && Pull(mft, decoder, w, h, out) &&
         SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) && SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)) && Pull(mft, decoder, w, h, out);
    sample->Release();
    return ok;
}
// Codes `luma` (w x h, multiples of 16) as one H.264 intra picture at `qp` and decodes it back into `decoded`.
bool RoundTrip(const std::vector<std::uint8_t>& luma, int w, int h, unsigned qp, std::vector<std::uint8_t>& decoded, std::size_t& bytes) {
    std::vector<std::uint8_t> nv12(luma); nv12.resize(static_cast<std::size_t>(w) * h * 3 / 2, 128);
    std::vector<std::uint8_t> stream;
    IMFTransform* encoder = nullptr; ICodecAPI* codec = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder))) &&
              SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&codec)));
    if (ok) {
        VARIANT v{}; v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_Quality;
        ok = SUCCEEDED(codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v));
        v.vt = VT_UI4; v.ulVal = 1; ok = ok && SUCCEEDED(codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v));
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; (void)codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        // Fixed QP (quality rate control): honoured only when set before the media types.
        v.vt = VT_UI8; v.ullVal = qp; ok = ok && SUCCEEDED(codec->SetValue(&CODECAPI_AVEncVideoEncodeQP, &v));
    }
    IMFMediaType* type = ok ? VideoType(MFVideoFormat_H264, w, h) : nullptr;
    if (type) {
        type->SetUINT32(MF_MT_AVG_BITRATE, 20000000); type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        ok = SUCCEEDED(encoder->SetOutputType(0, type, 0)); Drop(type);
    } else ok = false;
    type = ok ? VideoType(MFVideoFormat_NV12, w, h) : nullptr;
    if (type) { ok = SUCCEEDED(encoder->SetInputType(0, type, 0)); Drop(type); } else ok = false;
    ok = ok && Feed(encoder, nv12.data(), nv12.size(), false, w, h, stream) && !stream.empty();
    Drop(codec); Drop(encoder);
    bytes = stream.size();
    IMFTransform* decoder = nullptr;
    ok = ok && SUCCEEDED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder)));
    type = ok ? VideoType(MFVideoFormat_H264, w, h) : nullptr;
    if (type) { ok = SUCCEEDED(decoder->SetInputType(0, type, 0)) && SelectNv12(decoder); Drop(type); } else ok = false;
    decoded.clear();
    ok = ok && Feed(decoder, stream.data(), stream.size(), true, w, h, decoded) && decoded.size() == static_cast<std::size_t>(w) * h;
    Drop(decoder);
    return ok;
}

// A training or validation picture: the clean high-resolution target and its low-resolution inputs, clean (condition 0)
// and after each H.264 round trip. The low-resolution planes are cropped to multiples of 16 (macroblocks).
constexpr unsigned kQp[]{22, 26, 30};
constexpr unsigned kConditions = 1 + sizeof(kQp) / sizeof(kQp[0]);
struct Source { const Image* image = nullptr; int lw = 0, lh = 0; std::vector<std::uint8_t> lr[kConditions]; double bpp[kConditions]{}; };
bool Prepare(const Image& image, Source& s) {
    s.image = &image; s.lw = image.w / 2 / 16 * 16; s.lh = image.h / 2 / 16 * 16;
    s.lr[0].resize(static_cast<std::size_t>(s.lw) * s.lh);
    for (int j = 0; j < s.lh; ++j) for (int i = 0; i < s.lw; ++i) s.lr[0][static_cast<std::size_t>(j) * s.lw + i] = Decimated(image, i, j);
    s.bpp[0] = 8;
    for (unsigned q = 0; q < kConditions - 1; ++q) {
        std::size_t bytes = 0;
        if (!RoundTrip(s.lr[0], s.lw, s.lh, kQp[q], s.lr[q + 1], bytes)) { std::printf("H.264 round trip failed: %s, QP %u\n", image.name.c_str(), kQp[q]); return false; }
        s.bpp[q + 1] = 8.0 * static_cast<double>(bytes) / (static_cast<double>(s.lw) * s.lh);
    }
    return true;
}

constexpr int P = 28, F1 = P - 2, F2 = P - 4; // LR patch; conv1 over [1, P-1); conv2/conv3 and loss over [2, P-2).
constexpr unsigned kParams = sizeof(NeuralModel) / sizeof(float);
struct Patch { float lr[P][P]; float hr[2 * F2][2 * F2]; };
void Sample(const std::vector<Source>& images, const std::vector<Source>& procedural, Patch& patch) {
    const bool synthetic = Next() % 4 == 0;
    const auto& set = synthetic ? procedural : images;
    const Source& s = set[Next() % set.size()];
    const unsigned condition = Next() % 4 == 0 ? 0 : 1 + Next() % (kConditions - 1); // A quarter clean.
    const int li = 4 + static_cast<int>(Next() % static_cast<unsigned>(s.lw - P - 8)), lj = 4 + static_cast<int>(Next() % static_cast<unsigned>(s.lh - P - 8));
    for (int j = 0; j < P; ++j) for (int i = 0; i < P; ++i) patch.lr[j][i] = s.lr[condition][static_cast<std::size_t>(lj + j) * s.lw + li + i] / 255.0f;
    for (int y = 0; y < 2 * F2; ++y) for (int x = 0; x < 2 * F2; ++x) patch.hr[y][x] = s.image->At(2 * li + 4 + x, 2 * lj + 4 + y);
}
float Base(const Patch& p, int x, int y, int sx, int sy) { // Bilinear sub-pixel of LR pixel (x, y), as in the shader.
    const int nx = sx ? x + 1 : x - 1, ny = sy ? y + 1 : y - 1;
    return 0.5625f * p.lr[y][x] + 0.1875f * p.lr[y][nx] + 0.1875f * p.lr[ny][x] + 0.0625f * p.lr[ny][nx];
}
// Forward + backward for one patch; accumulates gradients (double) and returns the summed squared error.
double Gradient(const NeuralModel& m, const Patch& p, double* g) {
    static thread_local float z1[F1][F1][16], a1[F1][F1][16], z2[F2][F2][8], a2[F2][F2][8], d1[F1][F1][16];
    for (int y = 0; y < F1; ++y) for (int x = 0; x < F1; ++x) for (int c = 0; c < 16; ++c) {
        float s = m.conv1_bias[c];
        for (int k = 0; k < 9; ++k) s += m.conv1_weight[c][k] * p.lr[y + k / 3][x + k % 3];
        z1[y][x][c] = s; a1[y][x][c] = media::video::LeakyRelu(s); d1[y][x][c] = 0;
    }
    for (int y = 0; y < F2; ++y) for (int x = 0; x < F2; ++x) for (int o = 0; o < 8; ++o) {
        float s = m.conv2_bias[o];
        for (int c = 0; c < 16; ++c) for (int k = 0; k < 9; ++k) s += m.conv2_weight[o][c][k] * a1[y + k / 3][x + k % 3][c];
        z2[y][x][o] = s; a2[y][x][o] = media::video::LeakyRelu(s);
    }
    double* g1w = g; double* g1b = g1w + 144; double* g2w = g1b + 16; double* g2b = g2w + 1152; double* g3w = g2b + 8; double* g3b = g3w + 32;
    const float scale = 2.0f / (4.0f * F2 * F2);
    double loss = 0;
    for (int y = 0; y < F2; ++y) for (int x = 0; x < F2; ++x) {
        float df2[8]{};
        for (int s = 0; s < 4; ++s) {
            float r = m.conv3_bias[s];
            for (int c = 0; c < 8; ++c) r += m.conv3_weight[s][c] * a2[y][x][c];
            const int sx = s & 1, sy = s >> 1;
            const float error = Base(p, x + 2, y + 2, sx, sy) + r - p.hr[2 * y + sy][2 * x + sx];
            loss += static_cast<double>(error) * error;
            const float dr = scale * error;
            g3b[s] += dr;
            for (int c = 0; c < 8; ++c) { g3w[s * 8 + c] += dr * a2[y][x][c]; df2[c] += dr * m.conv3_weight[s][c]; }
        }
        for (int o = 0; o < 8; ++o) {
            const float dz = df2[o] * (z2[y][x][o] > 0 ? 1.0f : media::video::kLeakySlope);
            if (dz == 0) continue;
            g2b[o] += dz;
            for (int c = 0; c < 16; ++c) for (int k = 0; k < 9; ++k) {
                g2w[(o * 16 + c) * 9 + k] += dz * a1[y + k / 3][x + k % 3][c];
                d1[y + k / 3][x + k % 3][c] += dz * m.conv2_weight[o][c][k];
            }
        }
    }
    for (int y = 0; y < F1; ++y) for (int x = 0; x < F1; ++x) for (int c = 0; c < 16; ++c) {
        const float dz = d1[y][x][c] * (z1[y][x][c] > 0 ? 1.0f : media::video::kLeakySlope);
        if (dz == 0) continue;
        g1b[c] += dz;
        for (int k = 0; k < 9; ++k) g1w[c * 9 + k] += dz * p.lr[y + k / 3][x + k % 3];
    }
    return loss;
}
// Validation PSNR (full-range luma against the clean original) on the centre (up to) 384x384 input / 768x768 output of
// a source under one condition: bilinear, bicubic, then each model (with the deringing clamp, as played back).
std::vector<double> Validate(const std::vector<const NeuralModel*>& models, const Source& s, unsigned condition) {
    const int lw = (std::min)(s.lw, 384), lh = (std::min)(s.lh, 384), li = (s.lw - lw) / 2, lj = (s.lh - lh) / 2, hw = 2 * lw, hh = 2 * lh;
    std::vector<float> lr(static_cast<std::size_t>(lw) * lh), bl(static_cast<std::size_t>(hw) * hh);
    std::vector<std::vector<float>> nn(models.size(), std::vector<float>(bl.size()));
    for (int j = 0; j < lh; ++j) for (int i = 0; i < lw; ++i) lr[static_cast<std::size_t>(j) * lw + i] = s.lr[condition][static_cast<std::size_t>(lj + j) * s.lw + li + i] / 255.0f;
    const media::video::LumaView view{lr.data(), static_cast<unsigned>(lw), static_cast<unsigned>(lh), static_cast<std::size_t>(lw)};
    for (std::size_t m = 0; m < models.size(); ++m) media::video::NeuralUpscaleReference(view, *models[m], nn[m].data(), static_cast<std::size_t>(hw));
    media::video::BilinearUpscaleReference(view, bl.data(), static_cast<std::size_t>(hw));
    constexpr float cubic[2][4]{{-0.0234375f, 0.2265625f, 0.8671875f, -0.0703125f}, {-0.0703125f, 0.8671875f, 0.2265625f, -0.0234375f}};
    auto lrAt = [&](int x, int y) { return lr[static_cast<std::size_t>(std::clamp(y, 0, lh - 1)) * lw + std::clamp(x, 0, lw - 1)]; };
    std::vector<double> e(2 + models.size(), 0.0); std::size_t n = 0;
    for (int y = 8; y < hh - 8; ++y) for (int x = 8; x < hw - 8; ++x) {
        const int lx = x / 2, ly = y / 2, sx = x & 1, sy = y & 1;
        float bc = 0;
        for (int b = 0; b < 4; ++b) { float row = 0; for (int a = 0; a < 4; ++a) row += cubic[sx][a] * lrAt(lx + a - 2 + sx, ly + b - 2 + sy); bc += cubic[sy][b] * row; }
        const float truth = FromUnorm(s.image->At(2 * li + x, 2 * lj + y));
        const std::size_t at = static_cast<std::size_t>(y) * hw + x;
        auto add = [&](std::size_t k, float value) { const double d = FromUnorm(value) - truth; e[k] += d * d; };
        add(0, bl[at]); add(1, std::clamp(bc, 0.0f, 1.0f));
        for (std::size_t m = 0; m < models.size(); ++m) add(2 + m, nn[m][at]);
        ++n;
    }
    for (auto& sum : e) sum = 10 * std::log10(1.0 / (sum / static_cast<double>(n)));
    return e;
}
}
int main(int argc, char** argv) {
    if (argc < 2) { std::puts("Usage: media_train_neural <repository root> [steps]"); return 2; }
    const unsigned steps = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 40000;
    const bool evaluate_only = steps == 0;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) return 1;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 1;
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(L"C:\\Windows\\Web", fs::directory_options::skip_permission_denied)) {
        const auto ext = entry.path().extension().wstring();
        if (entry.is_regular_file() && (ext == L".jpg" || ext == L".png") && entry.path().wstring().find(L"\\4K\\") == std::wstring::npos) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<Image> train, validation;
    for (std::size_t i = 0; i < files.size(); ++i) { Image image; if (Load(wic, files[i], image)) (i % 4 == 3 ? validation : train).push_back(std::move(image)); }
    wic->Release();
    std::vector<Image> procedural, procedural_validation;
    for (unsigned i = 0; i < 24; ++i) procedural.push_back(Procedural(i));
    for (unsigned i = 0; i < 4; ++i) procedural_validation.push_back(Procedural(100 + i));
    std::printf("Photos: %zu training, %zu validation; procedural: %zu + %zu\n", train.size(), validation.size(), procedural.size(), procedural_validation.size());
    if (train.empty() || validation.empty()) return 1;
    // Low-resolution inputs, clean and through H.264 at each QP (the image vectors no longer change: Source points into them).
    auto prepare = [](const std::vector<Image>& images, std::vector<Source>& sources) {
        sources.resize(images.size());
        for (std::size_t i = 0; i < images.size(); ++i) if (!Prepare(images[i], sources[i])) return false;
        return true;
    };
    std::vector<Source> train_sources, validation_sources, procedural_sources, procedural_validation_sources;
    if (!prepare(train, train_sources) || !prepare(validation, validation_sources) || !prepare(procedural, procedural_sources) ||
        !prepare(procedural_validation, procedural_validation_sources)) return 1;
    double bpp[kConditions]{};
    for (const auto& s : validation_sources) for (unsigned c = 0; c < kConditions; ++c) bpp[c] += s.bpp[c] / static_cast<double>(validation_sources.size());
    std::printf("H.264 intra, mean bits per input pixel on the validation photos:");
    for (unsigned c = 1; c < kConditions; ++c) std::printf(" QP %u %.3f", kQp[c - 1], bpp[c]);
    std::printf("\n");
    // He initialisation; the last layer starts near zero, so training starts from bilinear.
    NeuralModel model{};
    for (auto& w : model.conv1_weight) for (auto& v : w) v = Normal() * std::sqrt(2.0f / 9);
    for (auto& o : model.conv2_weight) for (auto& c : o) for (auto& v : c) v = Normal() * std::sqrt(2.0f / 144);
    for (auto& s : model.conv3_weight) for (auto& v : s) v = Normal() * 1e-3f;
    constexpr unsigned kThreads = 8, kBatch = 32;
    std::vector<float> mean(kParams, 0), variance(kParams, 0);
    std::vector<Patch> patches(kBatch);
    std::vector<std::vector<double>> gradients(kThreads, std::vector<double>(kParams));
    std::vector<double> losses(kThreads);
    const auto start = GetTickCount64();
    double running = 0;
    for (unsigned step = 1; step <= steps; ++step) {
        for (auto& patch : patches) Sample(train_sources, procedural_sources, patch);
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < kThreads; ++t) workers.emplace_back([&, t] {
            std::fill(gradients[t].begin(), gradients[t].end(), 0.0); losses[t] = 0;
            for (unsigned b = t; b < kBatch; b += kThreads) losses[t] += Gradient(model, patches[b], gradients[t].data());
        });
        for (auto& w : workers) w.join();
        double loss = 0; for (unsigned t = 0; t < kThreads; ++t) loss += losses[t];
        running = step == 1 ? loss : 0.99 * running + 0.01 * loss;
        // Adam with cosine decay from 2e-3 to 2e-5.
        const float rate = 2e-5f + 0.5f * (2e-3f - 2e-5f) * (1 + std::cos(3.14159265f * step / steps));
        auto* w = reinterpret_cast<float*>(&model);
        const float c1 = 1 - std::pow(0.9f, static_cast<float>(step)), c2 = 1 - std::pow(0.999f, static_cast<float>(step));
        for (unsigned i = 0; i < kParams; ++i) {
            double gsum = 0; for (unsigned t = 0; t < kThreads; ++t) gsum += gradients[t][i];
            const float gi = static_cast<float>(gsum / kBatch);
            mean[i] = 0.9f * mean[i] + 0.1f * gi; variance[i] = 0.999f * variance[i] + 0.001f * gi * gi;
            w[i] -= rate * (mean[i] / c1) / (std::sqrt(variance[i] / c2) + 1e-8f);
        }
        if (step % 2000 == 0 || step == steps)
            std::printf("step %6u  loss/px %.3e  lr %.2e  %.0f s\n", step, running / (kBatch * 4.0 * F2 * F2), rate, (GetTickCount64() - start) / 1000.0);
    }
    // Held-out evaluation: bilinear, bicubic, the committed weights and (when trained) the new ones, per condition.
    std::vector<const NeuralModel*> models{&media::render::kNeuralWeights};
    if (!evaluate_only) models.push_back(&model);
    const std::size_t columns = 2 + models.size();
    std::string report;
    for (unsigned c = 0; c < kConditions; ++c) {
        std::vector<double> photos(columns, 0.0), synthetic(columns, 0.0);
        for (const auto& s : validation_sources) { const auto r = Validate(models, s, c); for (std::size_t k = 0; k < columns; ++k) photos[k] += r[k] / static_cast<double>(validation_sources.size()); }
        for (const auto& s : procedural_validation_sources) { const auto r = Validate(models, s, c); for (std::size_t k = 0; k < columns; ++k) synthetic[k] += r[k] / static_cast<double>(procedural_validation_sources.size()); }
        char name[32]; if (c == 0) std::snprintf(name, sizeof(name), "clean 8-bit"); else std::snprintf(name, sizeof(name), "H.264 QP %u", kQp[c - 1]);
        for (int set = 0; set < 2; ++set) {
            const auto& r = set == 0 ? photos : synthetic;
            char line[256];
            int n = std::snprintf(line, sizeof(line), "//   %-12s %-10s bilinear %.2f, bicubic %.2f, previous %.2f", name, set == 0 ? "photos" : "procedural", r[0], r[1], r[2]);
            if (columns > 3) n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), ", new %.2f", r[3]);
            std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " dB\n");
            std::printf("%s", line + 5); report += line;
        }
    }
    MFShutdown(); CoUninitialize();
    if (evaluate_only) return 0;
    // Header.
    const auto out_path = fs::path(argv[1]) / "include" / "media" / "render" / "NeuralWeights.hpp";
    FILE* out = nullptr; if (fopen_s(&out, out_path.string().c_str(), "wb") || !out) return 1;
    std::fprintf(out, "#pragma once\n// Generated by tools/train_neural.cpp (media_train_neural) - do not edit by hand.\n"
                      "// Micro-ESPCN 2x luma residual upscaler, %u parameters (%zu bytes). %u Adam steps, batch %u, seed fixed.\n"
                      "// Training data: %zu Windows photographs (C:\\Windows\\Web, 4K copies excluded) and 24 procedural images; inputs\n"
                      "// 2x Keys-bicubic-decimated and 8-bit-quantised, a quarter clean and the rest through an H.264 intra round trip at\n"
                      "// QP 22, 26 or 30 (Windows software codec; mean %.2f, %.2f, %.2f bits per input pixel on the validation photos).\n"
                      "// Held-out validation, mean PSNR of full-range luma against the clean original (previous = the weights these\n"
                      "// replaced, trained on clean input only):\n%s"
                      "#include \"media/video/NeuralUpscale.hpp\"\nnamespace media::render {\ninline constexpr video::NeuralModel kNeuralWeights{\n",
                 kParams, sizeof(NeuralModel), steps, kBatch, train.size(), bpp[1], bpp[2], bpp[3], report.c_str());
    auto list = [&](const float* values, unsigned n, unsigned per_line, const char* indent) {
        for (unsigned i = 0; i < n; ++i) std::fprintf(out, "%s%.9gf,%s", i % per_line == 0 ? indent : "", static_cast<double>(values[i]), i % per_line == per_line - 1 || i == n - 1 ? "\n" : " ");
    };
    std::fprintf(out, "    {\n"); for (auto& w : model.conv1_weight) { std::fprintf(out, "        {"); list(w, 9, 9, ""); std::fprintf(out, "        },\n"); } std::fprintf(out, "    },\n");
    std::fprintf(out, "    {\n"); list(model.conv1_bias, 16, 8, "        "); std::fprintf(out, "    },\n");
    std::fprintf(out, "    {\n");
    for (auto& o : model.conv2_weight) { std::fprintf(out, "        {\n"); for (auto& c : o) { std::fprintf(out, "            {"); list(c, 9, 9, ""); std::fprintf(out, "            },\n"); } std::fprintf(out, "        },\n"); }
    std::fprintf(out, "    },\n    {\n"); list(model.conv2_bias, 8, 8, "        "); std::fprintf(out, "    },\n");
    std::fprintf(out, "    {\n"); for (auto& s : model.conv3_weight) { std::fprintf(out, "        {"); list(s, 8, 8, ""); std::fprintf(out, "        },\n"); } std::fprintf(out, "    },\n");
    std::fprintf(out, "    {\n"); list(model.conv3_bias, 4, 4, "        "); std::fprintf(out, "    },\n};\n}\n");
    std::fclose(out);
    std::printf("Wrote %s\n", out_path.string().c_str());
    return 0;
}
