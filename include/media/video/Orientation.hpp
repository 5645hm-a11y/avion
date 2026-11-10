#pragma once
#include <cstdint>

namespace media::video {
// How a picture is shown turned (docs/picture.md, "Rotation"), portable, no allocation: the file's own orientation (its video
// track's tkhd matrix, IsobmffParser's TrackInfo::transform: a, b, c, d, a quarter turn or mirror of the axes), then the
// viewer's quarter turns clockwise, then a leveling turn of a few degrees. Picture coordinates throughout: x right, y down,
// so a positive turn is clockwise on the screen.
struct Orientation {
    std::int8_t file[4]{1, 0, 0, 1}; // tkhd: x' = a x + c y, y' = b x + d y.
    unsigned turns = 0;              // The viewer's quarter turns, clockwise (taken modulo 4).
    float level = 0;                 // Degrees, clockwise.
    // Whether `t` (a, b, c, d) is a quarter turn or mirror of the axes (entries -1, 0, 1, one per row and column).
    [[nodiscard]] static bool Axes(const std::int8_t t[4]) noexcept;
    // display = D source: D = (d[0] d[1]; d[2] d[3]), the file's matrix then the viewer's turns.
    void Display(int d[4]) const noexcept;
    [[nodiscard]] unsigned QuarterTurns() const noexcept; // Shown, 0..3 clockwise (a mirror taken out first).
    [[nodiscard]] bool Mirrored() const noexcept;
    [[nodiscard]] bool Across() const noexcept;           // An odd quarter turn: the picture's width is shown upright.
    [[nodiscard]] bool Upright() const noexcept;          // Shown as stored: no turn, no mirror, no level.
    // The conversion's map (ColorConstants kRotate): an output picture of w x h pixels showing a region of rw x rh samples
    // turned; output pixel centre X -> the region's samples q = M X + t (from its origin). Leveled, the view is zoomed in by
    // cos + sin x the longer side over the shorter, the least that keeps the turned picture's corners inside the region.
    void Map(double w, double h, double rw, double rh, double m[4], double t[2]) const noexcept;
    // A point of the picture as shown (fractions, 0.5 the centre) at the same place in the stored picture's fractions;
    // a direction likewise (the leveling turn left out: a few degrees).
    void ToStored(float& x, float& y) const noexcept;
    void DirectionToStored(float& dx, float& dy) const noexcept;
};
}
