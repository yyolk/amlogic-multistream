#pragma once

#include <string>
#include <vector>

// LayoutEngine chooses an equal-cell grid for a 1–9 stream wall and turns
// that grid into integer pixel rectangles.
//
// The functions are pure (no global state) and do not touch decode, MQTT,
// or DRM. StreamManager can call suggestLayout(); a renderer can call
// compute() and draw one texture per Tile.
//
// suggestLayout(streamCount) — grid name, clamped outside 1–9:
//   streamCount <= 1  →  "1x1"   (0 and negative are included)
//   2                 →  "1x2"   side by side
//   3 or 4            →  "2x2"   3 streams leave the bottom-right cell empty
//   5 or 6            →  "2x3"   5 streams leave the last cell empty
//   7, 8, 9           →  "3x3"   unused cells stay empty
//   streamCount > 9   →  "3x3"   clamped; compute() places at most 9 tiles
//
// Names are rows, then columns, then a lowercase "x" ("2x3" is 2 rows by
// 3 columns). Cells are filled row-major: left to right, top to bottom.
// Stream slot i sits at row (i / cols), column (i % cols).
//
// compute(width, height, streamCount, layoutOverride) — rectangles:
//   - layoutOverride empty: use suggestLayout(streamCount).
//   - otherwise the override must be "<rows>x<cols>" with each side in
//     1..kMaxGridSide. That accepts every suggestLayout() name plus the
//     rotations it does not emit ("2x1", "1x3", "3x1", "3x2"). Anything
//     else fails (ok == false, no tiles).
//   - Viewport width and height must be positive; otherwise the call fails.
//   - streamCount < 0 is treated as 0 (a valid result with no tiles).
//   - The number of tiles is min(max(streamCount, 0), rows * cols).
//     Extra streams get no rectangle. In auto mode the grid is at most
//     3x3, so at most kMaxStreams tiles are produced.
//
// Geometry (no gutters, no per-tile letterboxing):
//   - Every grid cell is a solid rectangle. The engine does not fit a
//     video aspect ratio inside the cell; that belongs to the renderer.
//   - Cells meet edge to edge. They do not overlap, and the full grid
//     (occupied cells plus any unused ones) covers the viewport exactly.
//   - Column c occupies x in [c * width / cols, (c + 1) * width / cols),
//     using integer division. Rows use the same split on height. Neighboring
//     cells differ by at most one pixel, and the last cell ends on the
//     viewport edge. If a side is smaller than the number of rows or
//     columns, some cells have zero width or height; they still do not
//     double-cover a pixel.
//   - Unused cells (streamCount < rows * cols) stay empty. Occupied tiles
//     keep the equal-cell size and are not stretched across the hole.
//     Example: 3 streams in "2x2" leave the bottom-right quarter empty.

class LayoutEngine {
public:
    // Largest equal grid this player will build. suggestLayout() never
    // returns a name above this, and parseLayout() rejects larger sides.
    static constexpr int kMaxGridSide = 3;
    static constexpr int kMaxStreams = kMaxGridSide * kMaxGridSide;

    struct Rect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    // One stream slot placed in a grid cell.
    struct Tile {
        int index = 0;  // 0-based stream slot, also the row-major cell index
        int row = 0;
        int col = 0;
        Rect rect;
    };

    // ok == false leaves name empty, rows/cols 0, and tiles empty.
    // streamCount is the number of tiles produced (after clamping), not
    // necessarily the count the caller asked for.
    struct Result {
        std::string name;
        int rows = 0;
        int cols = 0;
        int streamCount = 0;
        std::vector<Tile> tiles;
        bool ok = false;
        std::string error;
    };

    // Suggested grid name. See the file comment for the table.
    // Counts below 1 yield "1x1". Counts above kMaxStreams yield "3x3".
    static std::string suggestLayout(int streamCount);

    // Parse "<rows>x<cols>". Each side must be a single digit in
    // 1..kMaxGridSide, separated by a lowercase 'x' (no spaces).
    // On failure rows and cols are left unchanged and the function
    // returns false.
    static bool parseLayout(const std::string& name, int& rows, int& cols);

    // Per-stream rectangles for a viewport. Pass layoutOverride = "" to
    // take suggestLayout(streamCount). See the file comment for the
    // cover / empty-cell policy and the failure cases.
    static Result compute(int viewportWidth,
                          int viewportHeight,
                          int streamCount,
                          const std::string& layoutOverride = "");
};
