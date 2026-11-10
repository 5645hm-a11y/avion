#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>

namespace media::video {
// The camera's rotation since the previous picture (an IMU), radians, by its effect on the picture: yaw positive turning
// right (the scene moves left), pitch positive tilting up (the scene moves down), roll positive turning clockwise as seen
// from behind the camera (the scene turns counter-clockwise).
struct EgoMotion { float yaw = 0, pitch = 0, roll = 0; bool valid = false; };

// One of the objects perception keeps track of (2026-10-31; Perception's class comment, "Objects"): an approaching surface, a
// salient region of the tile grid, or both, with a track id that stays with it from picture to picture.
struct PerceivedObject {
    std::uint32_t track_id = 0;     // 1, 2, ...: the same object keeps its id while it is seen; ids are not reused (Configure starts over).
    float x = 0.5f, y = 0.5f;       // Its centre, fractions of the picture.
    float width = 0, height = 0;    // Its extent, fractions of the picture.
    float vx = 0, vy = 0;           // Its track's velocity, picture widths / heights per second (0 on its first picture).
    float ttc_seconds = -1;         // Approaching: time to contact, 2 / divergence (-1: not approaching).
    float divergence = 0;           // That expansion, 1/s (0: none).
    float saliency = 0;             // Its tiles' mean saliency, 0..1.
    float energy = 0;               // Its tiles' saliency added up (in tiles): how much of the scene's attention it holds.
    std::uint32_t support = 0;      // Accepted expansion windows on it.
    std::uint32_t tiles = 0;        // Salient tiles in it (0: an approach outside the saliency mask).
    std::uint32_t age = 0;          // Pictures its track has been seen in (1: new).
    bool approaching = false;       // An approach confirmed on its track (ttc_seconds is set).
    bool fovea = false;             // Its approach was found by the fovea's finer tiles.
    // Hidden behind another object it is crossing (2026-11-01): its track's prediction (where its motion puts it), kept while
    // its prediction stays on the other's region, at most kTrackHold unseen; tiles 0.
    bool occluded = false;
};
inline constexpr unsigned kMaxPerceivedObjects = 4;

struct PerceptionResult {
    std::int64_t pts_100ns = -1;
    std::uint32_t sequence = 0;     // Pictures pushed since Configure.
    bool flow = false;              // Flow measured (there was an earlier picture to compare with).
    bool ego = false;               // The flow is the scene's own: the camera's measured rotation was taken out.
    float motion_x = 0, motion_y = 0; // The dominant motion (median of the reliable tiles), picture widths / heights per second.
    float coverage = 0;             // The share of tiles whose flow is reliable (textured, unambiguous).
    bool looming = false;           // Something expands toward the camera: an isotropic, shear-free expansion of the flow.
    float divergence = 0;           // That expansion, 1/s (div v; 0 when none).
    float ttc_seconds = -1;         // Time to contact, 2 / divergence (-1: nothing approaching).
    float hazard_x = 0.5f, hazard_y = 0.5f; // Where (its focus of expansion), fractions of the picture.
    bool fovea = false;             // The approach was found by the fovea's finer tiles (a small or distant obstacle).
    bool ground = false;            // The camera travels forward over a ground plane: its flow is expected, not an approach.
    float horizon_y = -1, focus_x = -1; // That ground's horizon and focus of expansion, fractions of the picture (-1: none).
    std::uint32_t ground_windows = 0; // Expansions the ground explained (not reported as looming).
    std::uint32_t support = 0;      // Accepted windows on the strongest expansion's object (looming needs kMinSupport, 2026-10-29).
    std::uint32_t salient = 0;      // Tiles in the saliency mask.
    float salient_fraction = 0;     // Of the kTiles x kTiles.
    // Up to kMaxPerceivedObjects objects, the most urgent first: confirmed approaches by time to contact (soonest first), then
    // the rest by saliency energy (2026-10-31).
    PerceivedObject objects[kMaxPerceivedObjects]{};
    std::uint32_t object_count = 0;
};

// Physical-world perception from the picture's luma (docs/picture.md, "Perception"), portable, for robots and cameras as
// much as for films: no model, a few milliseconds of CPU, no allocation after Configure.
// - Cells: the picture as kCells x kCells cell means (16x the 8-bit code, area means over integer bounds), kept for
//   kHistory pictures with their times and the camera's accumulated rotation.
// - Flow: each of the kTiles x kTiles tiles (kTileCells x kTileCells cells) is matched in an earlier picture (the newest one
//   at least kBaseline old, else the previous one) by zero-mean SAD within +-kSearch cells of where the camera's rotation
//   moved it (IMU), refined by a parabola per axis; a tile is reliable when it is textured (kMinTexture), its best match is
//   not on the search's edge (retried against the previous picture), well below the search's mean cost (a match, not the
//   least bad of many: content entering the picture has none) and the cost rises in both axes (no aperture problem).
//   Velocities are pixels per second with the camera's rotation taken out.
// - The search (2026-10-29; every position of the window before): seeded where the IMU puts the tile and where its own flow
//   in the previous picture and its left and upper neighbours' in this one predict it (kMaxSeeds), the small diamond (the
//   four neighbours) descends from the cheapest seed to a local minimum. When that costs under kNoiseFloor of the tile's own
//   texture (sum |v - mean| of its cells; an unrelated position costs about that or more) the search ends there; else the
//   window on a lattice (every kLattice cells, kLatticeSide^2 positions) gives the mean cost the test above uses and a second
//   descent from its cheapest point, for a motion no seed predicted. Each position's cost once per tile; every level (scalar,
//   SSE2, AVX2) visits the same positions in the same order: the same results, bit for bit.
// - Looming: a least-squares affine fit of the flow over every 5x5-tile window (at least 8 reliable tiles); an approach is
//   an expansion (div = a11 + a22 > kMinDivergence) that is isotropic (both axes expand, the smaller at least kIsotropy of the
//   larger) and shear-free (the off-diagonal terms small) and explains the flow (the fit's residual small beside it; the
//   worst-fitting tiles are left out once),
//   which a translating edge or corner, or noise, is not. The strongest gives the divergence, TTC = 2 / div (a surface
//   approaching at constant speed, flow (x - x0) / TTC) and the hazard at its focus of expansion. Each tile's flow is placed
//   where its texture is (its cells' centres weighted by their gradient), which is what its match follows: a tile holding an
//   obstacle's edge over a flat background moves with the edge, not with its own centre (2026-10-27). A displacement over a
//   baseline dt, at the tiles' present positions, is x (1 - Z / (Z + v dt)): the fit's expansion is 2 / (TTC + dt), so the
//   divergence is corrected to d / (1 - d dt / 2) with dt the window's mean baseline (2026-10-27; before, every time to
//   contact read dt late: 0.1 s on the 90 ms baseline, 0.23 s on the fovea's at 30 pictures a second).
// - The fovea (2026-10-27): the central half of the picture again, as kFine x kFine overlapping tiles (kTileCells x kTileCells
//   cells every kFineStride cells: twice as dense), matched against the oldest picture held (the longest baseline: a slow
//   expansion moves the tiles twice as far as over kBaseline), its 5x5-tile windows fitted as above. A distant obstacle
//   covering two coarse tiles fills a fine window; on a 5x5 coarse window it was a third of the tiles and failed the fit.
// - The ground (2026-10-27; wherever the horizon is, 2026-10-28): a camera travelling forward over a ground plane sees the
//   ground flow out of a focus of expansion (x0, yh: on the horizon), faster toward the bottom: u = k (x - x0)(y - yh) + r,
//   v = k (y - yh)^2 (+ a constant: pitch). Fitted to coarse rows from the bottom up (v as a quadratic in y gives k and the
//   horizon yh, then u with its own ku gives x0 and r; the worst tiles left out once), and accepted when it explains that
//   flow (residual under 0.75 of it), k > 0, the horizon is above the rows, ku / k is a ground's (0.5 to 1.6: an obstacle's
//   isotropic expansion fitted this way gives about 2) and the ground moves at least kGroundFlow at the bottom. The rows: the
//   most of them from the bottom (kMaxGroundRows down to 3) that one ground explains, so the horizon may be anywhere in the
//   picture (a camera pitched up or down, a window cut from a larger picture); rows reaching past the horizon put its vertex
//   among them and are refused. The last ground's horizon, moved by the IMU's pitch (focal x pitch), says which rows to try
//   first. A picture whose rows do not fit keeps the last ground (also moved by the pitch) while the tiles below its horizon,
//   however few rows (a horizon low in the picture leaves one to three), still fit it with its offsets refitted (2026-10-29:
//   the low-horizon window found a ground on 45 of 292 pictures, now 292); else, while travel is steady (an obstacle over the
//   rows), for kGroundHold. Then a window below the horizon whose flow the
//   ground explains (its residual against the ground under 0.75x the window's expansion, or under 1.5x the window's own
//   affine residual: as good as a fit of its own) is the ground, not an approach; an obstacle standing on it expands about its
//   own centre (v changes sign across it), which the ground cannot explain (on the beach footage: ground windows 0.5-0.8x,
//   the obstacle's at least 1.05x its expansion and 2.4x its residual).
// - The approach reported: the strongest accepted window says where; its divergence is the median of every accepted window's
//   (both levels) whose focus is within a tenth of the picture of it (the same object): the strongest of hundreds of noisy
//   windows alone overestimated the expansion (time to contact 10-20 % early on real footage). A rigid surface coming closer
//   expands about one centre, so many overlapping windows see it: at least kMinSupport on the object (an obstacle two coarse
//   tiles wide had 12 or more on the beach footage; water washing in, reflections on the wet sand and foam passed every shape
//   test in one or two windows at a time and were 33 of 292 pictures of the high-horizon window). And the object persists:
//   its centre within a tenth of the picture of the last one found, at most kPersistence before (2026-10-29).
// - Saliency: per tile, 0.55 temporal change (above the picture's median, the camera's noise or its own motion) + 0.30 flow
//   speed + 0.15 edge density, each scaled to 0..1; the mask is the tiles at kSalient or more: what a robot's slower
//   vision needs to look at. The change is measured against where the camera's rotation (IMU) moved the tile since the
//   previous picture (between cells; cells that came from outside the picture are not compared), so shake is not change, and
//   from 1.5x a floor that is the picture's median change scaled by the tile's texture against the median tile's, on a scale
//   that grows with it (2026-10-22: before, a shaken camera made every edge salient). A still camera's floor is about 0.
// - Objects (2026-10-31; docs/picture.md, "Several objects at once"): more than one thing moves or approaches at once. The
//   accepted expansion windows are taken apart by non-maximum suppression: the strongest window's object (every window whose
//   focus is within a tenth of the picture of it, as above) is one approach when it has kMinSupport windows, and its windows
//   are set aside, then the strongest left, up to kMaxPerceivedObjects. The saliency mask is taken apart into its connected
//   regions (tiles up to kObjectGap apart each way: an approaching surface's interior changes little, its salient
//   edges come in pieces a tile apart) of at least kMinObjectTiles tiles and at most kMaxObjectShare of the grid (the scene itself, a
//   travelling camera's whole field, is no object); an approach whose focus lies in or next to a region is that region (its
//   extent and its saliency), the others stand alone. Each object is then given the track it continues: the nearest
//   track's prediction (its last centre moved by its velocity) within kTrackGate of the picture (plus half the object's size),
//   nearest pairs first; a new id otherwise; tracks unseen for kTrackHold are let go (up to kTracks held). An approach is
//   confirmed (approaching, ttc_seconds) when its track had one at most kPersistence before (the single approach above is
//   confirmed by the same rule). The table: confirmed approaches by time to contact, then the rest by energy.
// - Crossings (2026-11-01; docs/picture.md, "Objects that cross"): two objects passing within kObjectGap tiles of each other
//   are one salient region, and its centre is neither's. Each track predicts where it is now at constant velocity: the
//   least-squares line through its last kPath measured centres (Extrapolate; the centres alone jump by a tile as a region grows,
//   and the tiles' flow reads about 0.7 of an object's speed where its edges cross a still background), before that its flow
//   (the saliency-weighted mean of its reliable tiles'). A region holding the predictions of two or more tracks (within a tile of
//   its bounds) is split between them: each tile goes to the track whose prediction is nearest, against its size, and whose
//   velocity its flow matches (kFlowWeight). Such a part is a measurement of its track when its flow moves as the track does (the
//   same direction within kAgreeCos, kAgreeSpeed of its speed either way); else (the other object's tiles, the hidden one's few)
//   the track goes on along its line, and a part of fewer than kMinObjectTiles tiles leaves it hidden
//   (PerceivedObject::occluded: reported at its prediction). The other objects are given their tracks by the least total cost (every assignment of up to
//   kMaxPerceivedObjects objects to kTracks tracks): the distance to the prediction against the gate, plus kTurnWeight x
//   (1 - cos) / 2 of the object's flow against the track's velocity, kAreaWeight x |ln| of their tile counts' ratio (over
//   ln 4), kShapeWeight x |ln| of their aspect ratios', and kLumaWeight x their tiles' mean codes' difference over kLumaScale
//   (each capped at 1); an object costs kNewTrack unassigned, more than any pair inside the gate.
// One thread. Results are deterministic (integer cells and costs, fits in float without contraction).
class Perception final {
public:
    static constexpr unsigned kTiles = 16, kTileCells = 8, kCells = kTiles * kTileCells, kHistory = 8;
    static constexpr int kSearch = 6;
    // The search (2026-10-29): seeds, then the small diamond; the lattice only when the best seed's descent stays above the
    // noise floor (see the class comment).
    static constexpr unsigned kMaxSeeds = 3;              // Predictions besides the IMU's: the tile's last flow, its left and upper neighbours'.
    // The coarse level (2026-11-05, API 17; docs/benchmarks.md, "Motion beyond the search"): the cells as a kCoarse x kCoarse grid
    // (each the mean of kCoarseRatio x kCoarseRatio cells), kept with every picture of the history. Each tile's kCoarseSupport x
    // kCoarseSupport coarse cells (its own and a ring of context) are matched over every position within +-kCoarseSearch coarse
    // cells (4x the fine window's reach; every second position, then the 3 x 3 around the cheapest) when the fine search around the
    // prediction did not settle (no match, or one above the noise floor and above a quarter of its window's mean cost); a clear
    // minimum beyond kCoarseRecentre cells of the prediction is searched by the fine window centred on it, and that match is kept
    // when it lies within kCoarseAgree cells of the coarse one and the first window has none or one more than twice as costly.
    static constexpr unsigned kCoarseRatio = 4, kCoarse = kCells / kCoarseRatio, kCoarseSupport = 4;
    static constexpr int kCoarseSearch = 5, kCoarseRecentre = kSearch / 2;
    static constexpr float kCoarseAgree = 2.0f; // Cells each way: the fine match in that window must refine the coarse one.
    static constexpr int kLattice = 4, kLatticeSide = 4;  // Every 4 cells of the 13 x 13 window (corners included): 16 positions.
    static constexpr float kNoiseFloor = 0.35f;           // A match's cost under this share of the tile's own texture ends the search.
    static constexpr unsigned kMinSize = 128;             // Pictures smaller than kCells samples a side are not measured.
    static constexpr std::int64_t kBaseline = 900000;     // 90 ms: a longer baseline measures slow expansions better.
    static constexpr float kMinTexture = 1.5f;            // Codes: the tile's mean absolute deviation.
    static constexpr float kMinDivergence = 0.2f;         // 1/s: a time to contact within 10 s.
    static constexpr float kIsotropy = 0.5f;              // A window's smaller expansion over its larger at least (0.35 until 2026-10-29).
    static constexpr unsigned kMinSupport = 4;            // Accepted windows on one object, at least (2026-10-29).
    static constexpr std::int64_t kPersistence = 1500000; // 0.15 s: the same object found before, at most this long ago.
    static constexpr float kMinShift = 0.25f;             // Cells: the least expansion displacement over the baseline (rms),
    static constexpr float kEgoShare = 0.05f;             // plus this share of what the IMU's rotation moved the tiles over it.
    static constexpr float kSalient = 0.25f;
    static constexpr unsigned kFine = 15, kFineStride = 4, kFineOrigin = kCells / 4; // The fovea: cells 32..96 each way.
    static constexpr float kGroundFlow = 20.0f;           // px/s: the least ground motion at the bottom that counts as travel.
    static constexpr unsigned kMaxGroundRows = 10;        // Coarse rows a ground is fitted to at most (3 at least).
    static constexpr std::int64_t kGroundHold = 5000000;  // 0.5 s: steady travel; a picture whose rows do not fit keeps the last ground.
    // Objects (2026-10-31).
    static constexpr unsigned kMinObjectTiles = 2;        // Salient tiles a region needs to be an object.
    static constexpr int kObjectGap = 2;                  // Salient tiles this close each way (one tile between) are one region.
    static constexpr float kMaxObjectShare = 0.4f;        // A region larger than this share of the grid is the scene, not an object.
    static constexpr float kTrackGate = 0.15f;            // Fractions of the picture: how far from its prediction a track is continued.
    static constexpr std::int64_t kTrackHold = 5000000;   // 0.5 s: a track unseen for longer is let go.
    static constexpr unsigned kTracks = 8;                // Tracks held at once.
    // Crossings (2026-11-01): the association's terms (see the class comment).
    static constexpr float kFlowWeight = 0.5f;            // A tile's flow against a track's velocity, in splitting a region.
    static constexpr float kTurnWeight = 0.6f, kAreaWeight = 0.3f, kShapeWeight = 0.15f, kLumaWeight = 0.5f;
    static constexpr float kLumaScale = 40.0f;            // Codes: a difference of mean luma this large costs kLumaWeight.
    static constexpr float kNewTrack = 3.0f;              // An object no track takes (above any pair's cost: 1 + 1.55).
    static constexpr float kMinSpeed = 0.02f;             // Picture widths per second: slower has no direction to compare.
    static constexpr float kAgreeCos = 0.8f, kAgreeSpeed = 1.8f; // A shared part's flow moving as its track does.
    // `pyramid`: with the coarse level's grids (SetPyramid(true) before Configure; 2026-11-05, about 48 KiB more).
    [[nodiscard]] static constexpr std::uint64_t ArenaBytes(unsigned width, bool pyramid = false) noexcept {
        return std::uint64_t{kHistory} * kCells * kCells * 2 + (pyramid ? std::uint64_t{kHistory} * (kCoarse * kCoarse * 2 + (kCoarse + 1) * (kCoarse + 1) * 4) : 0) +
               std::uint64_t{kCells} * kCells * 4 + width + 4096;
    }
    // Takes its buffers from `arena` (ArenaBytes). False for a picture smaller than kMinSize or larger than 8192 samples.
    [[nodiscard]] bool Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, float horizontal_fov_radians) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return history_ != nullptr; }
    void Reset() noexcept; // A cut, a seek: nothing to compare with.
    // One picture, in presentation order: its 8-bit luma (width x height as configured).
    const PerceptionResult& Push(const std::uint8_t* luma, unsigned stride, std::int64_t pts_100ns, const EgoMotion& ego) noexcept;
    [[nodiscard]] const PerceptionResult& Result() const noexcept { return result_; }
    // Per tile (row-major, kTiles x kTiles), of the last picture.
    [[nodiscard]] bool FlowValid(unsigned tile) const noexcept { return valid_[tile] != 0; }
    [[nodiscard]] float FlowX(unsigned tile) const noexcept { return flow_x_[tile]; } // Pixels per second.
    [[nodiscard]] float FlowY(unsigned tile) const noexcept { return flow_y_[tile]; }
    [[nodiscard]] std::uint8_t Saliency(unsigned tile) const noexcept { return saliency_[tile]; } // 0..255.
    [[nodiscard]] bool Salient(unsigned tile) const noexcept { return saliency_[tile] >= static_cast<std::uint8_t>(kSalient * 255.0f + 0.5f); }
    [[nodiscard]] unsigned Width() const noexcept { return width_; }
    // The search's costs (MatchTile) and the cells use SSE2 where the target has it (x86-64), the costs AVX2 where the processor
    // and the system have it too, unless built with MEDIA_PERCEPTION_SCALAR. Level(): 0 scalar, 1 SSE2, 2 AVX2. ForceLevel asks
    // for one (clamped to what is available; tests: every level gives the same results, bit for bit). Process-wide, set before use.
    static unsigned ForceLevel(unsigned level) noexcept;
    // The coarse level on, or off (the default: the single-level search of 2026-10-29). Measured 2026-11-05 on MPI Sintel (1,041 pairs):
    // it reports more of the fast tiles (32+ px a frame: 9.2 % to 14.4 %) but their error stays that of no motion (52.8 to 51.6 px),
    // the error on its own tiles rises (2.41 to 2.73 px) and it costs about 110 us a picture.
    // Its grids are reserved by Configure when it is on then (ArenaBytes(width, true)); turned on later without them it does nothing.
    void SetPyramid(bool on) noexcept { pyramid_ = on; }
    // With the coarse level (diagnostic, 2026-11-05): a tile the long baseline found nothing for is searched over the picture before.
    void SetShortBaseline(bool on) noexcept { short_baseline_ = on; }
    [[nodiscard]] bool Pyramid() const noexcept { return pyramid_; }
    [[nodiscard]] static unsigned Level() noexcept;
    [[nodiscard]] unsigned Height() const noexcept { return height_; }
private:
    struct Frame { std::int64_t pts = 0; double yaw = 0, pitch = 0, roll = 0; };
    void MakeCells(const std::uint8_t* luma, unsigned stride, std::uint16_t* out) noexcept;
    // The coarse motion (full cells, `then` to `now`) of the tile whose first cell is (bx, by), searched within +-kCoarseSearch coarse
    // cells of (cx, cy) (full cells); false without a clear minimum.
    bool CoarseMatch(const std::uint16_t* now, unsigned age, unsigned bx, unsigned by, int cx, int cy, float& dx, float& dy) const noexcept;
    // The costs of `n` positions of the search for the tile at (bx, by), the block's first cells in `then` at (rx, ry), all on
    // the grid (scalar, SSE2 or AVX2: the same integers); `sum`: the tile's cells'.
    static void Costs(const std::uint16_t* now, const std::uint16_t* then, int bx, int by, std::int32_t sum, const int* rx, const int* ry, unsigned n, std::int64_t* cost) noexcept;
    // The best displacement of the tile whose first cell is (bx, by) of `now` in `then`, searched within +-kSearch cells of
    // (cx, cy), from there and from `seeds` (seed_count displacements, x then y, cells); false when unreliable.
    bool MatchTile(const std::uint16_t* now, const std::uint16_t* then, unsigned bx, unsigned by, int cx, int cy, const int* seeds, unsigned seed_count,
                   float& dx, float& dy, bool& edge, std::int64_t* best_cost = nullptr, bool* settled = nullptr) const noexcept;
    // Velocities that predict a tile's flow (px/s, the camera's rotation out), the search's seeds.
    struct Prediction { float vx[kMaxSeeds], vy[kMaxSeeds]; unsigned count; };
    // One tile's flow (px/s, the camera's rotation out) against the pictures `ages` back, the first that matches; false: none.
    // `seconds`: the baseline it was measured over; (x, y): where the tile's texture is (pixels), which its match follows;
    // `ego`: how far the camera's rotation (IMU) moved it over the baseline (pixels), taken out of the flow.
    bool TileFlow(const std::uint16_t* now, unsigned bx, unsigned by, const unsigned* ages, unsigned count, std::int64_t pts, const Prediction& predicted, float& vx, float& vy,
                  float& seconds, float& x, float& y, float& ego, bool coarse = false) const noexcept;
    // The seeds of tile t of a columns-wide field: its own last flow (last_valid, last_x, last_y) and the flows of its left and
    // upper neighbours found in this picture (valid, vx, vy).
    static Prediction Predict(unsigned t, unsigned columns, const std::uint8_t* last_valid, const float* last_x, const float* last_y, const std::uint8_t* valid,
                              const float* vx, const float* vy) noexcept;
    void Saliency(const std::uint16_t* now, const std::uint16_t* before) noexcept;
    // A tile grid for the looming fit: `columns` x `rows` tiles, the first cell of tile (i, j) at (x0 + i step, y0 + j step).
    struct TileField { unsigned columns, rows; const float *vx, *vy, *seconds, *x, *y, *ego; const std::uint8_t* valid; bool fine; };
    void Looming(const TileField&) noexcept; // Adds every accepted window to the candidates.
    void Approach(std::int64_t pts) noexcept; // The strongest candidate's object: result_'s looming fields.
    void Objects(std::int64_t pts) noexcept;  // The object table: result_.objects (the candidates and the saliency mask, then the tracks).
    void Ground(std::int64_t pts, float pitch) noexcept; // This picture's ground (Fit), or the held one; `pitch`: the IMU's since the last.
    bool Fit(unsigned first_row) noexcept;   // ground_ from the coarse rows from first_row down; false when they are no ground.
    struct GroundModel;
    bool Confirm(GroundModel& model) const noexcept; // Whether the tiles below its horizon still fit `model` (its offsets refitted).
    [[nodiscard]] const std::uint16_t* Grid(unsigned age) const noexcept { return history_ + static_cast<std::size_t>((newest_ + kHistory - age) % kHistory) * kCells * kCells; }

    unsigned width_ = 0, height_ = 0;
    float focal_ = 0;                        // Pixels per radian at the centre: (width / 2) / tan(fov / 2).
    std::uint16_t* history_ = nullptr;       // kHistory grids of kCells x kCells.
    std::uint16_t* coarse_ = nullptr;        // Their coarse grids, kCoarse x kCoarse each,
    std::int32_t* coarse_sums_ = nullptr;    // and each one's summed-area table ((kCoarse + 1)^2: a block's sum in four reads).
    bool pyramid_ = false, short_baseline_ = false; // Off by default: measured on Sintel it did not fix fast motion (docs/benchmarks.md, 2026-11-05).
    [[nodiscard]] unsigned Slot(unsigned age) const noexcept { return (newest_ + kHistory - age) % kHistory; }
    [[nodiscard]] const std::uint16_t* CoarseGrid(unsigned age) const noexcept { return coarse_ + static_cast<std::size_t>(Slot(age)) * kCoarse * kCoarse; }
    [[nodiscard]] const std::int32_t* CoarseSums(unsigned age) const noexcept { return coarse_sums_ + static_cast<std::size_t>(Slot(age)) * (kCoarse + 1) * (kCoarse + 1); }
    std::uint32_t* sums_ = nullptr;          // A grid's sums while it is made.
    std::uint8_t* column_ = nullptr;         // The cell column of every sample column.
    std::uint32_t row_bounds_[kCells + 1]{}, column_bounds_[kCells + 1]{};
    unsigned cell_rows_ = 0, cell_columns_ = 0;                // A cell's lines and samples (or one more of each).
    struct Divide { std::uint64_t magic = 1; std::uint32_t half = 0; unsigned shift = 0; } divide_[2][2]{}; // [taller][wider].
    Frame frames_[kHistory]{};
    unsigned newest_ = 0, count_ = 0;        // Grids held (count_ <= kHistory).
    double yaw_ = 0, pitch_ = 0, roll_ = 0;  // The camera's accumulated rotation.
    PerceptionResult result_{};
    float flow_x_[kTiles * kTiles]{}, flow_y_[kTiles * kTiles]{}, flow_s_[kTiles * kTiles]{}; // _s: the baseline (seconds).
    float flow_px_[kTiles * kTiles]{}, flow_py_[kTiles * kTiles]{};                           // Where each tile's texture is.
    float flow_e_[kTiles * kTiles]{}, fine_e_[kFine * kFine]{};                               // The IMU's displacement taken out.
    std::uint8_t valid_[kTiles * kTiles]{}, saliency_[kTiles * kTiles]{};
    float fine_x_[kFine * kFine]{}, fine_y_[kFine * kFine]{}, fine_s_[kFine * kFine]{}, fine_px_[kFine * kFine]{}, fine_py_[kFine * kFine]{};
    std::uint8_t fine_valid_[kFine * kFine]{};
    // The previous picture's flows (the search's seeds).
    float last_x_[kTiles * kTiles]{}, last_y_[kTiles * kTiles]{}, last_fine_x_[kFine * kFine]{}, last_fine_y_[kFine * kFine]{};
    std::uint8_t last_valid_[kTiles * kTiles]{}, last_fine_valid_[kFine * kFine]{};
    struct GroundModel { bool valid = false; float k = 0, horizon = 0, focus = 0, offset_u = 0, offset_v = 0, ku = 0; } ground_{}, held_{}; // Pixels.
    std::int64_t held_pts_ = 0;              // When held_ (the last ground fitted) was fitted.
    struct Candidate { float divergence, x, y; bool fine; };
    Candidate candidates_[kTiles * kTiles + kFine * kFine]{};
    unsigned candidate_count_ = 0;
    struct Object { bool valid = false; std::int64_t pts = 0; float x = 0, y = 0; } object_{}; // The last approach found (pixels), reported or not.
    // The objects' tracks (pixels, pixels per second); id 0: a free slot. approach: when it last had an approach (INT64_MIN: never).
    // pts: the time of its state (x, y: measured, or its prediction while hidden); seen: when it was last measured. Its look
    // (2026-11-01): tiles, width and height (pixels), its tiles' mean code.
    // path: its last kPath measured centres (a ring; path_count of them, the newest at path_head - 1), its prediction's line.
    static constexpr unsigned kPath = 8;
    struct Track {
        std::uint32_t id = 0, age = 0; float x = 0, y = 0, vx = 0, vy = 0; std::int64_t pts = 0, seen = 0, approach = INT64_MIN;
        float tiles = 0, width = 0, height = 0, luma = -1;
        std::int64_t path_pts[kPath]{}; float path_x[kPath]{}, path_y[kPath]{}; unsigned path_count = 0, path_head = 0;
    };
    // Where track `t` is at `pts` and its velocity (pixels, px/s): the least-squares line through its path's centres of the last
    // second when they are three or more over 80 ms at least; else its last position moved by its velocity (false).
    static bool Extrapolate(const Track& t, std::int64_t pts, float& x, float& y, float& vx, float& vy) noexcept;
    Track tracks_[kTracks]{};
    std::uint32_t next_track_ = 1;
    float ego_yaw_ = 0, ego_pitch_ = 0, ego_roll_ = 0; // The camera's rotation since the previous picture (saliency's registration).
};
}
