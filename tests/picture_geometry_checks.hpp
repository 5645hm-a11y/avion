#pragma once
// Viewport arithmetic of media/video/PictureGeometry.hpp, shared by the portable suite and the GPU suite.
// The including file defines CHECK(x).
#include "media/video/PictureGeometry.hpp"

namespace geometry_checks {
using namespace media::video;
// Every identity here is also true at compile time.
static_assert(SampleAspectFromIdc(1) == SampleAspect{1, 1} && SampleAspectFromIdc(4) == SampleAspect{16, 11});
static_assert(SampleAspectFromIdc(255, 4, 3) == SampleAspect{4, 3} && SampleAspectFromIdc(0) == SampleAspect{0, 0});
static_assert(DisplaySize(1440, 1080, {4, 3}) == PictureSize{1920, 1080});
static_assert(Viewport({1920, 1080}, {1920, 1200}) == PictureRect{0, 60, 1920, 1080});

inline void ViewportMath() {
    // Table E-1: every listed idc, the reserved ones, and the explicit pair.
    constexpr SampleAspect table[17]{{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
                                     {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}};
    for (unsigned idc = 0; idc < 17; ++idc) CHECK(SampleAspectFromIdc(idc, 7, 9) == table[idc]);
    for (unsigned idc = 17; idc < 255; ++idc) CHECK(!Specified(SampleAspectFromIdc(idc, 7, 9)));
    CHECK(SampleAspectFromIdc(255, 64, 45) == (SampleAspect{64, 45}));
    CHECK(!Specified(SampleAspectFromIdc(255, 0, 45)) && !Specified(SampleAspectFromIdc(255, 64, 0)));
    // Lowest terms; damage is square.
    CHECK(Normalize({40, 30}) == (SampleAspect{4, 3}) && Normalize({65535, 65535}) == (SampleAspect{1, 1}));
    CHECK(Normalize({0, 5}) == (SampleAspect{1, 1}) && Normalize({9, 1}) == (SampleAspect{1, 1}) && Normalize({1, 9}) == (SampleAspect{1, 1}));
    CHECK(Normalize({8, 1}) == (SampleAspect{8, 1}) && Normalize({2, 16}) == (SampleAspect{1, 8}));
    // Which description is believed.
    AspectSource source = AspectSource::Square;
    CHECK(ResolveSampleAspect({0, 0}, {0, 0}, &source) == (SampleAspect{1, 1}) && source == AspectSource::Square);
    CHECK(ResolveSampleAspect({0, 0}, {16, 11}, &source) == (SampleAspect{16, 11}) && source == AspectSource::Bitstream);
    CHECK(ResolveSampleAspect({1, 1}, {16, 11}, &source) == (SampleAspect{16, 11}) && source == AspectSource::Bitstream); // A default pasp.
    CHECK(ResolveSampleAspect({4, 3}, {16, 11}, &source) == (SampleAspect{4, 3}) && source == AspectSource::Container);
    CHECK(ResolveSampleAspect({8, 6}, {0, 0}, &source) == (SampleAspect{4, 3}) && source == AspectSource::Container);
    CHECK(ResolveSampleAspect({100, 1}, {4, 3}, &source) == (SampleAspect{4, 3}) && source == AspectSource::Bitstream);  // Damaged pasp.
    CHECK(ResolveSampleAspect({5, 5}, {3, 3}, &source) == (SampleAspect{1, 1}) && source == AspectSource::Square);
    // Display sizes of the anamorphic formats in use. Wide samples widen; tall samples make the picture taller.
    struct Case { std::uint32_t w, h; SampleAspect sar; PictureSize shown; };
    constexpr Case cases[]{
        {720, 576, {16, 11}, {1047, 576}},   // PAL 16:9 (the full 720 columns; 702 of them are the 16:9 picture).
        {720, 576, {12, 11}, {785, 576}},    // PAL 4:3.
        {720, 480, {40, 33}, {873, 480}},    // NTSC 16:9.
        {720, 480, {10, 11}, {720, 528}},    // NTSC 4:3: tall samples.
        {704, 480, {10, 11}, {704, 528}},
        {1440, 1080, {4, 3}, {1920, 1080}},  // HDV, XDCAM.
        {1280, 1080, {3, 2}, {1920, 1080}},  // DVCPRO HD.
        {960, 720, {4, 3}, {1280, 720}},
        {1920, 1080, {1, 1}, {1920, 1080}},
        {1920, 800, {0, 0}, {1920, 800}},    // Not said: square.
        {3840, 1080, {1, 2}, {3840, 2160}},
        {101, 67, {4, 3}, {135, 67}},        // Odd crops round to nearest.
        {60, 44, {4, 3}, {80, 44}},
        {1, 1, {8, 1}, {8, 1}},
        {16000, 9000, {2, 1}, {16384, 9000}} // Clamped to the largest side handled.
    };
    for (const auto& c : cases) CHECK(DisplaySize(c.w, c.h, c.sar) == c.shown);
    // Fitting a shape into a window, and the bars around it.
    CHECK(Viewport({1920, 1080}, {1920, 1080}) == (PictureRect{0, 0, 1920, 1080}));
    CHECK(Viewport({1920, 1080}, {1920, 1200}) == (PictureRect{0, 60, 1920, 1080}));      // 16:9 on 16:10: letterbox.
    CHECK(Viewport({1440, 1080}, {1920, 1080}) == (PictureRect{240, 0, 1440, 1080}));     // 4:3 on 16:9: pillarbox.
    CHECK(Viewport({1920, 800}, {1280, 720}) == (PictureRect{0, 93, 1280, 533}));         // 2.40:1 scaled down.
    CHECK(Viewport(DisplaySize(720, 576, {16, 11}), {1920, 1080}) == (PictureRect{0, 12, 1920, 1056})); // PAL 16:9 is 1.818:1.
    CHECK(Viewport(DisplaySize(1440, 1080, {4, 3}), {1280, 720}) == (PictureRect{0, 0, 1280, 720}));    // Anamorphic HD fills 16:9.
    CHECK(Viewport(DisplaySize(720, 480, {10, 11}), {1440, 1080}) == (PictureRect{0, 12, 1440, 1056}));
    CHECK(Viewport({1000, 1000}, {300, 200}) == (PictureRect{50, 0, 200, 200}));
    CHECK(Viewport({1920, 1080}, {1, 1}) == (PictureRect{0, 0, 1, 1}) && Viewport({0, 0}, {640, 360}) == (PictureRect{0, 0, 640, 360}));
    // Within two pixels of the bound it snaps: 1920x1078 content in a 1920x1080 window has no sliver bars.
    CHECK(Viewport({1920, 1078}, {1920, 1080}) == (PictureRect{0, 0, 1920, 1080}));
    CHECK(Viewport({1920, 1076}, {1920, 1080}) == (PictureRect{0, 2, 1920, 1076}));
    // The fitted shape keeps the aspect within one pixel, for every window of a sweep and four shapes.
    unsigned wrong = 0;
    constexpr PictureSize shapes[]{{1920, 1080}, {1047, 576}, {720, 528}, {135, 67}};
    for (const auto& shape : shapes)
        for (std::uint32_t w = 97; w <= 4000; w += 131) for (std::uint32_t h = 61; h <= 2400; h += 97) {
            const auto r = Viewport(shape, {w, h});
            const bool inside = r.width <= w && r.height <= h && r.x + r.width <= w && r.y + r.height <= h && r.width && r.height;
            const bool touches = r.width == w || r.height == h;
            // |width/height - shape| stays within one pixel of either side (or the two-pixel snap).
            const double ideal_w = static_cast<double>(r.height) * shape.width / shape.height, ideal_h = static_cast<double>(r.width) * shape.height / shape.width;
            const bool shaped = (ideal_w - r.width < 3.5 && r.width - ideal_w < 3.5) || (ideal_h - r.height < 3.5 && r.height - ideal_h < 3.5);
            const bool centred = r.x == (w - r.width) / 2 && r.y == (h - r.height) / 2;
            if (!inside || !touches || !shaped || !centred) ++wrong;
        }
    CHECK(wrong == 0);
    // Plane bounds of a crop: which texels are picture. A chroma texel is when either column (row) it covers is.
    const auto luma = LumaBounds(3, 5, 104, 72); const auto chroma = ChromaBounds(3, 5, 104, 72);
    CHECK(luma.low_x == 3 && luma.low_y == 5 && luma.high_x == 103 && luma.high_y == 71);
    CHECK(chroma.low_x == 1 && chroma.low_y == 2 && chroma.high_x == 51 && chroma.high_y == 35);
    const auto even = ChromaBounds(0, 0, 1920, 1080);
    CHECK(even.low_x == 0 && even.low_y == 0 && even.high_x == 959 && even.high_y == 539);
}
}
