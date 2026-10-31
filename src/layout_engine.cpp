#include "layout_engine.h"

#include <cstddef>

static_assert(LayoutEngine::kMaxGridSide == 3,
              "suggestLayout() names are written for a 3x3 maximum grid");
static_assert(LayoutEngine::kMaxStreams == 9,
              "suggestLayout() clamps anything above 9 streams to 3x3");

namespace {

int splitStart(int index, int count, int extent) {
    // long long keeps (index * extent) defined for large viewports.
    return static_cast<int>(
        (static_cast<long long>(index) * static_cast<long long>(extent)) / count);
}

LayoutEngine::Rect cellRect(int col,
                            int row,
                            int cols,
                            int rows,
                            int width,
                            int height) {
    LayoutEngine::Rect rect;
    rect.x = splitStart(col, cols, width);
    rect.y = splitStart(row, rows, height);
    rect.width = splitStart(col + 1, cols, width) - rect.x;
    rect.height = splitStart(row + 1, rows, height) - rect.y;
    return rect;
}

}  // namespace

std::string LayoutEngine::suggestLayout(int streamCount) {
    // Same table the skeleton shipped. The final branch clamps 7..9 and
    // anything above kMaxStreams (9) to the 3x3 grid.
    if (streamCount <= 1) return "1x1";
    if (streamCount <= 2) return "1x2";
    if (streamCount <= 4) return "2x2";
    if (streamCount <= 6) return "2x3";
    return "3x3";
}

bool LayoutEngine::parseLayout(const std::string& name, int& rows, int& cols) {
    // Exactly "RxC": one digit, lowercase x, one digit. Sides are 1..3.
    if (name.size() != 3 || name[1] != 'x') {
        return false;
    }
    const char rowChar = name[0];
    const char colChar = name[2];
    const char maxDigit = static_cast<char>('0' + kMaxGridSide);
    if (rowChar < '1' || rowChar > maxDigit || colChar < '1' || colChar > maxDigit) {
        return false;
    }
    rows = rowChar - '0';
    cols = colChar - '0';
    return true;
}

LayoutEngine::Result LayoutEngine::compute(int viewportWidth,
                                           int viewportHeight,
                                           int streamCount,
                                           const std::string& layoutOverride) {
    Result result;

    if (viewportWidth <= 0 || viewportHeight <= 0) {
        result.error = "viewport width and height must be positive";
        return result;
    }

    const std::string name =
        layoutOverride.empty() ? suggestLayout(streamCount) : layoutOverride;

    int rows = 0;
    int cols = 0;
    if (!parseLayout(name, rows, cols)) {
        result.error = "invalid layout \"" + name +
                       "\" (expected <rows>x<cols> with each side in 1.." +
                       std::to_string(kMaxGridSide) + ")";
        return result;
    }

    const int capacity = rows * cols;
    int placed = streamCount;
    if (placed < 0) {
        placed = 0;
    }
    if (placed > capacity) {
        placed = capacity;
    }

    result.name = name;
    result.rows = rows;
    result.cols = cols;
    result.streamCount = placed;
    result.tiles.reserve(static_cast<std::size_t>(placed));

    for (int i = 0; i < placed; ++i) {
        Tile tile;
        tile.index = i;
        tile.row = i / cols;
        tile.col = i % cols;
        tile.rect = cellRect(tile.col, tile.row, cols, rows, viewportWidth, viewportHeight);
        result.tiles.push_back(tile);
    }

    result.ok = true;
    return result;
}
