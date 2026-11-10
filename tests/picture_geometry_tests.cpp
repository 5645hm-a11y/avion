// Portable picture description: viewport arithmetic (crop, sample aspect ratio, letterboxing) and colour
// description resolution (container against bitstream, and what is inferred when neither says).
#include "media/video/ColorDescription.hpp"
#include "media/video/PictureGeometry.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
}
#include "picture_geometry_checks.hpp"
namespace {
using namespace media::video;
ColorDescription Described(unsigned primaries, unsigned transfer, unsigned matrix, bool full, bool range = true) {
    ColorDescription c; c.primaries = static_cast<std::uint8_t>(primaries); c.transfer = static_cast<std::uint8_t>(transfer);
    c.matrix = static_cast<std::uint8_t>(matrix); c.full_range = full; c.described = true; c.range_described = range; return c;
}
void Colour() {
    const ColorDescription none;
    // Nothing said anywhere: inferred from the picture height. SD is BT.601 (625-line primaries at 576 lines), HD BT.709.
    auto c = ResolveColor(none, none, 480);
    CHECK(c.matrix == 6 && c.primaries == 6 && c.transfer == 6 && !c.full_range && !c.described);
    c = ResolveColor(none, none, 576); CHECK(c.matrix == 5 && c.primaries == 5 && c.transfer == 6);
    c = ResolveColor(none, none, 64); CHECK(c.matrix == 6);
    c = ResolveColor(none, none, 577); CHECK(c.matrix == 1 && c.primaries == 1 && c.transfer == 1);
    c = ResolveColor(none, none, 720); CHECK(c.matrix == 1);
    c = ResolveColor(none, none, 2160); CHECK(c.matrix == 1 && c.primaries == 1);
    // The bitstream alone is taken as written, whatever the size.
    c = ResolveColor(none, Described(5, 6, 5, false), 1080); CHECK(c.matrix == 5 && c.primaries == 5 && c.transfer == 6 && c.described);
    c = ResolveColor(none, Described(9, 16, 9, false), 2160); CHECK(c.matrix == 9 && c.primaries == 9 && c.transfer == 16 && !c.full_range);
    c = ResolveColor(none, Described(1, 1, 1, true), 480); CHECK(c.matrix == 1 && c.full_range && c.range_described);
    // The container wins, code point by code point, where it says something.
    c = ResolveColor(Described(1, 1, 1, false), Described(5, 6, 5, true), 480);
    CHECK(c.matrix == 1 && c.primaries == 1 && c.transfer == 1 && !c.full_range);
    c = ResolveColor(Described(9, 2, 2, true), Described(1, 18, 9, false), 1080);
    CHECK(c.primaries == 9 && c.transfer == 18 && c.matrix == 9 && c.full_range);
    // nclc has no range flag: the bitstream's range stands.
    c = ResolveColor(Described(1, 1, 1, false, false), Described(2, 2, 2, true), 1080);
    CHECK(c.matrix == 1 && c.full_range && c.range_described);
    c = ResolveColor(Described(1, 1, 1, true, false), Described(2, 2, 2, false, false), 1080);
    CHECK(!c.full_range && !c.range_described);
    // A container that carries a description of unspecified values does not erase the bitstream's.
    c = ResolveColor(Described(2, 2, 2, false), Described(9, 16, 9, false), 2160); CHECK(c.matrix == 9 && c.transfer == 16 && c.primaries == 9);
    // BT.2020 primaries with no matrix imply the BT.2020 matrix (and the other way round), at any size.
    c = ResolveColor(none, Described(9, 16, 2, false), 480); CHECK(c.matrix == 9);
    c = ResolveColor(none, Described(2, 2, 9, false), 480); CHECK(c.primaries == 9 && c.transfer == 14);
    // Reserved and out-of-table code points are unspecified, then inferred.
    c = ResolveColor(none, Described(0, 3, 3, false), 1080); CHECK(c.primaries == 1 && c.transfer == 1 && c.matrix == 1);
    c = ResolveColor(none, Described(200, 99, 77, false), 480); CHECK(c.primaries == 6 && c.transfer == 6 && c.matrix == 6);
    CHECK(KnownMatrix(0) == 0 && KnownMatrix(14) == 14 && KnownMatrix(15) == 2 && KnownPrimaries(22) == 22 && KnownPrimaries(13) == 2 && KnownTransfer(18) == 18 && KnownTransfer(19) == 2);
    // Every combination resolves to something specified, and the packed form round-trips.
    unsigned unresolved = 0, unpacked = 0;
    for (unsigned p = 0; p < 24; ++p) for (unsigned t = 0; t < 20; ++t) for (unsigned m = 0; m < 16; ++m) for (unsigned h : {144u, 576u, 1080u}) {
        const auto r = ResolveColor(p % 3 ? Described(p, 2, m, false) : none, Described(2, t, m, (p & 1) != 0), h);
        if (r.primaries == 2 || r.transfer == 2 || r.matrix == 2) ++unresolved;
        const auto back = UnpackColor(PackColor(r));
        if (back.primaries != r.primaries || back.transfer != r.transfer || back.matrix != r.matrix || back.full_range != r.full_range) ++unpacked;
    }
    CHECK(unresolved == 0 && unpacked == 0);
    CHECK(PackColor(Described(9, 16, 9, true)) == (9u | 256u | (16u << 16) | (9u << 24)));
    // What the conversion shaders implement.
    CHECK(ConvertibleMatrix(1) && ConvertibleMatrix(5) && ConvertibleMatrix(6) && ConvertibleMatrix(9));
    CHECK(!ConvertibleMatrix(0) && !ConvertibleMatrix(2) && !ConvertibleMatrix(8) && !ConvertibleMatrix(10) && !ConvertibleMatrix(14));
    CHECK(std::strcmp(PrimariesName(9), "BT.2020") == 0 && std::strcmp(TransferName(16), "PQ") == 0 && std::strcmp(MatrixName(1), "BT.709") == 0 &&
          std::strcmp(MatrixName(200), "unspecified") == 0);
}
}
int main() {
    allocation_probe::Begin();
    geometry_checks::ViewportMath();
    Colour();
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0);
    std::printf("Picture geometry: %s (%d failures, %llu allocations)\n", failures ? "FAIL" : "PASS", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
