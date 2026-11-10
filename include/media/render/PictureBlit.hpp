#pragma once

namespace media::render {
// Root constants of PictureBlit.hlsl: where the picture lies in its texture, in normalized texture coordinates.
// `extent` scales the quad's 0..1 coordinates onto the picture; `limit` is the centre of the picture's last texel
// in each axis. Sampling is clamped to it, so bilinear filtering at the right and bottom edges never blends in the
// texels beyond the picture (an RGB slot is as large as the largest window; what lies beyond the picture in it is
// whatever an earlier, larger picture left there).
// `origin` and `span` place the view in the picture (fractions of the picture): the quad's coordinate u shows the
// picture at origin + u * span. The whole picture is origin 0, span 1. A picture converted for another zoom region
// than the one asked for now is shown through it (docs/picture.md, "Live zoom"): the part of the view that the picture
// does not hold is black.
struct BlitConstants { float extent[2]; float limit[2]; float origin[2]; float span[2]; };
static_assert(sizeof(BlitConstants) == 32);
[[nodiscard]] constexpr BlitConstants MakeBlitConstants(unsigned picture_width, unsigned picture_height, unsigned texture_width,
                                                       unsigned texture_height) noexcept {
    const float w = static_cast<float>(texture_width), h = static_cast<float>(texture_height);
    return {{static_cast<float>(picture_width) / w, static_cast<float>(picture_height) / h},
            {(static_cast<float>(picture_width) - 0.5f) / w, (static_cast<float>(picture_height) - 0.5f) / h}, {0, 0}, {1, 1}};
}
// A rectangle of source samples (right and bottom exclusive): the region a picture was converted from.
struct SampleRect { unsigned left = 0, top = 0, right = 0, bottom = 0; };
// The view of `wanted` in a picture converted from `converted`: origin and span as fractions of that picture. Equal
// rectangles give the whole picture (0, 1). Both must be non-empty.
constexpr void ViewOf(const SampleRect& converted, const SampleRect& wanted, BlitConstants& c) noexcept {
    const double w = static_cast<double>(converted.right) - converted.left, h = static_cast<double>(converted.bottom) - converted.top;
    if (w <= 0 || h <= 0 || wanted.right <= wanted.left || wanted.bottom <= wanted.top) { c.origin[0] = c.origin[1] = 0; c.span[0] = c.span[1] = 1; return; }
    c.origin[0] = static_cast<float>((static_cast<double>(wanted.left) - converted.left) / w);
    c.origin[1] = static_cast<float>((static_cast<double>(wanted.top) - converted.top) / h);
    c.span[0] = static_cast<float>((static_cast<double>(wanted.right) - wanted.left) / w);
    c.span[1] = static_cast<float>((static_cast<double>(wanted.bottom) - wanted.top) / h);
}
constexpr bool operator==(const SampleRect& a, const SampleRect& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}
