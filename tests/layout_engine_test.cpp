#include "layout_engine.h"

#include <iostream>
#include <string>
#include <vector>

// Assert-style checks that keep going so one run reports every failure.
// The process exits non-zero if any check fails.

namespace {

int g_failures = 0;
const char* g_case = "";

void check(bool cond, const char* expr, int line) {
    if (cond) {
        return;
    }
    std::cerr << "FAIL " << g_case << " " << expr << " (" << __FILE__ << ":" << line << ")\n";
    ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

using Rect = LayoutEngine::Rect;
using Tile = LayoutEngine::Tile;
using Result = LayoutEngine::Result;

bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool positiveOverlap(const Rect& a, const Rect& b) {
    const long long ax2 = static_cast<long long>(a.x) + a.width;
    const long long ay2 = static_cast<long long>(a.y) + a.height;
    const long long bx2 = static_cast<long long>(b.x) + b.width;
    const long long by2 = static_cast<long long>(b.y) + b.height;
    const long long ix1 = a.x > b.x ? a.x : b.x;
    const long long iy1 = a.y > b.y ? a.y : b.y;
    const long long ix2 = ax2 < bx2 ? ax2 : bx2;
    const long long iy2 = ay2 < by2 ? ay2 : by2;
    return ix1 < ix2 && iy1 < iy2;
}

long long tileArea(const std::vector<Tile>& tiles) {
    long long area = 0;
    for (const Tile& tile : tiles) {
        area += static_cast<long long>(tile.rect.width) * tile.rect.height;
    }
    return area;
}

void expectInside(int width, int height, const std::vector<Tile>& tiles) {
    for (const Tile& tile : tiles) {
        CHECK(tile.rect.width >= 0);
        CHECK(tile.rect.height >= 0);
        CHECK(tile.rect.x >= 0);
        CHECK(tile.rect.y >= 0);
        CHECK(static_cast<long long>(tile.rect.x) + tile.rect.width <= width);
        CHECK(static_cast<long long>(tile.rect.y) + tile.rect.height <= height);
    }
}

void expectNoOverlap(const std::vector<Tile>& tiles) {
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        for (std::size_t j = i + 1; j < tiles.size(); ++j) {
            CHECK(!positiveOverlap(tiles[i].rect, tiles[j].rect));
        }
    }
}

// A full grid must partition the viewport: inside, no shared pixels,
// areas sum to the viewport, neighbors share an edge, and the outer
// edges sit on the viewport border.
void expectPartition(int width, int height, const Result& layout) {
    CHECK(layout.ok);
    const int cells = layout.rows * layout.cols;
    CHECK(static_cast<int>(layout.tiles.size()) == cells);
    CHECK(layout.streamCount == cells);
    if (!layout.ok || static_cast<int>(layout.tiles.size()) != cells || layout.tiles.empty()) {
        return;
    }
    expectInside(width, height, layout.tiles);
    expectNoOverlap(layout.tiles);
    CHECK(tileArea(layout.tiles) == static_cast<long long>(width) * height);

    CHECK(layout.tiles.front().rect.x == 0);
    CHECK(layout.tiles.front().rect.y == 0);
    const Rect& last = layout.tiles.back().rect;
    CHECK(static_cast<long long>(last.x) + last.width == width);
    CHECK(static_cast<long long>(last.y) + last.height == height);

    const int floorW = width / layout.cols;
    const int ceilW = floorW + (width % layout.cols == 0 ? 0 : 1);
    const int floorH = height / layout.rows;
    const int ceilH = floorH + (height % layout.rows == 0 ? 0 : 1);

    for (const Tile& tile : layout.tiles) {
        CHECK(tile.rect.width == floorW || tile.rect.width == ceilW);
        CHECK(tile.rect.height == floorH || tile.rect.height == ceilH);
        CHECK(tile.index == tile.row * layout.cols + tile.col);
    }

    for (int row = 0; row < layout.rows; ++row) {
        for (int col = 0; col < layout.cols; ++col) {
            const Tile& tile = layout.tiles[static_cast<std::size_t>(row * layout.cols + col)];
            CHECK(tile.row == row);
            CHECK(tile.col == col);
            if (col + 1 < layout.cols) {
                const Tile& right =
                    layout.tiles[static_cast<std::size_t>(row * layout.cols + col + 1)];
                CHECK(tile.rect.x + tile.rect.width == right.rect.x);
                CHECK(tile.rect.y == right.rect.y);
                CHECK(tile.rect.height == right.rect.height);
            }
            if (row + 1 < layout.rows) {
                const Tile& below =
                    layout.tiles[static_cast<std::size_t>((row + 1) * layout.cols + col)];
                CHECK(tile.rect.y + tile.rect.height == below.rect.y);
                CHECK(tile.rect.x == below.rect.x);
                CHECK(tile.rect.width == below.rect.width);
            }
        }
    }
}

void expectBitmapCover(int width, int height, const std::vector<Tile>& tiles) {
    std::vector<int> cover(static_cast<std::size_t>(width * height), 0);
    for (const Tile& tile : tiles) {
        for (int y = tile.rect.y; y < tile.rect.y + tile.rect.height; ++y) {
            for (int x = tile.rect.x; x < tile.rect.x + tile.rect.width; ++x) {
                if (x < 0 || y < 0 || x >= width || y >= height) {
                    CHECK(false);
                    continue;
                }
                cover[static_cast<std::size_t>(y * width + x)] += 1;
            }
        }
    }
    for (int hits : cover) {
        CHECK(hits == 1);
    }
}

void expectFailure(const Result& layout) {
    CHECK(!layout.ok);
    CHECK(!layout.error.empty());
    CHECK(layout.name.empty());
    CHECK(layout.rows == 0);
    CHECK(layout.cols == 0);
    CHECK(layout.streamCount == 0);
    CHECK(layout.tiles.empty());
}

void testSuggestLayout() {
    g_case = "suggestLayout";
    CHECK(LayoutEngine::suggestLayout(-4) == "1x1");
    CHECK(LayoutEngine::suggestLayout(0) == "1x1");
    CHECK(LayoutEngine::suggestLayout(1) == "1x1");
    CHECK(LayoutEngine::suggestLayout(2) == "1x2");
    CHECK(LayoutEngine::suggestLayout(3) == "2x2");
    CHECK(LayoutEngine::suggestLayout(4) == "2x2");
    CHECK(LayoutEngine::suggestLayout(5) == "2x3");
    CHECK(LayoutEngine::suggestLayout(6) == "2x3");
    CHECK(LayoutEngine::suggestLayout(7) == "3x3");
    CHECK(LayoutEngine::suggestLayout(8) == "3x3");
    CHECK(LayoutEngine::suggestLayout(9) == "3x3");
    CHECK(LayoutEngine::suggestLayout(10) == "3x3");
    CHECK(LayoutEngine::suggestLayout(100) == "3x3");

    CHECK(LayoutEngine::kMaxStreams == 9);
    CHECK(LayoutEngine::kMaxGridSide == 3);

    for (int n = -2; n <= 12; ++n) {
        int rows = -1;
        int cols = -1;
        const std::string name = LayoutEngine::suggestLayout(n);
        CHECK(LayoutEngine::parseLayout(name, rows, cols));
        CHECK(rows >= 1 && rows <= LayoutEngine::kMaxGridSide);
        CHECK(cols >= 1 && cols <= LayoutEngine::kMaxGridSide);
        CHECK(name == std::to_string(rows) + "x" + std::to_string(cols));
    }
}

void testParseLayout() {
    g_case = "parseLayout";
    const char* accepted[] = {"1x1", "1x2", "1x3", "2x1", "2x2", "2x3", "3x1", "3x2", "3x3"};
    for (const char* name : accepted) {
        int rows = 0;
        int cols = 0;
        CHECK(LayoutEngine::parseLayout(name, rows, cols));
        CHECK(rows == name[0] - '0');
        CHECK(cols == name[2] - '0');
    }

    const char* rejected[] = {
        "",        "2X2",   "2 x 2", "2x",    "x2",   "22",   "2*2", "2x3 ",
        " 2x3",    "4x4",   "0x1",   "2x0",   "10x1", "1x10", "9x9", "3x4",
        "4x3",     "abc",   "2x2x2", "\t2x2",
    };
    for (const char* name : rejected) {
        int rows = 7;
        int cols = 8;
        CHECK(!LayoutEngine::parseLayout(name, rows, cols));
        CHECK(rows == 7);
        CHECK(cols == 8);
    }
}

void testHandCalculated() {
    g_case = "1920x1080 2x2";
    const Result grid2 = LayoutEngine::compute(1920, 1080, 4);
    expectPartition(1920, 1080, grid2);
    CHECK(grid2.name == "2x2");
    const Rect expect2[] = {
        {0, 0, 960, 540},
        {960, 0, 960, 540},
        {0, 540, 960, 540},
        {960, 540, 960, 540},
    };
    for (int i = 0; i < 4; ++i) {
        CHECK(sameRect(grid2.tiles[static_cast<std::size_t>(i)].rect, expect2[i]));
    }

    g_case = "1920x1080 1x2";
    const Result side = LayoutEngine::compute(1920, 1080, 2);
    expectPartition(1920, 1080, side);
    CHECK(side.name == "1x2");
    CHECK(sameRect(side.tiles[0].rect, Rect{0, 0, 960, 1080}));
    CHECK(sameRect(side.tiles[1].rect, Rect{960, 0, 960, 1080}));

    g_case = "1920x1080 1x1";
    const Result single = LayoutEngine::compute(1920, 1080, 1);
    expectPartition(1920, 1080, single);
    CHECK(single.name == "1x1");
    CHECK(sameRect(single.tiles[0].rect, Rect{0, 0, 1920, 1080}));

    g_case = "1920x1080 2x3";
    const Result grid6 = LayoutEngine::compute(1920, 1080, 6);
    expectPartition(1920, 1080, grid6);
    CHECK(grid6.name == "2x3");
    CHECK(sameRect(grid6.tiles[0].rect, Rect{0, 0, 640, 540}));
    CHECK(sameRect(grid6.tiles[2].rect, Rect{1280, 0, 640, 540}));
    CHECK(sameRect(grid6.tiles[5].rect, Rect{1280, 540, 640, 540}));

    g_case = "1920x1080 3x3";
    const Result grid9 = LayoutEngine::compute(1920, 1080, 9);
    expectPartition(1920, 1080, grid9);
    CHECK(grid9.name == "3x3");
    CHECK(sameRect(grid9.tiles[0].rect, Rect{0, 0, 640, 360}));
    CHECK(sameRect(grid9.tiles[4].rect, Rect{640, 360, 640, 360}));
    CHECK(sameRect(grid9.tiles[8].rect, Rect{1280, 720, 640, 360}));

    // 1280 / 3 leaves a remainder. Widths are 426, 427, 427.
    g_case = "1280x720 3x3 remainder";
    const Result odd = LayoutEngine::compute(1280, 720, 9);
    expectPartition(1280, 720, odd);
    CHECK(sameRect(odd.tiles[0].rect, Rect{0, 0, 426, 240}));
    CHECK(sameRect(odd.tiles[1].rect, Rect{426, 0, 427, 240}));
    CHECK(sameRect(odd.tiles[2].rect, Rect{853, 0, 427, 240}));
    CHECK(sameRect(odd.tiles[3].rect, Rect{0, 240, 426, 240}));
    CHECK(sameRect(odd.tiles[8].rect, Rect{853, 480, 427, 240}));

    // Both axes leave a remainder: 100x70, 2x3.
    g_case = "100x70 2x3";
    const Result awkward = LayoutEngine::compute(100, 70, 6, "2x3");
    expectPartition(100, 70, awkward);
    CHECK(sameRect(awkward.tiles[0].rect, Rect{0, 0, 33, 35}));
    CHECK(sameRect(awkward.tiles[1].rect, Rect{33, 0, 33, 35}));
    CHECK(sameRect(awkward.tiles[2].rect, Rect{66, 0, 34, 35}));
    CHECK(sameRect(awkward.tiles[3].rect, Rect{0, 35, 33, 35}));
    CHECK(sameRect(awkward.tiles[5].rect, Rect{66, 35, 34, 35}));
    expectBitmapCover(100, 70, awkward.tiles);
}

void testPartialGridsDoNotStretch() {
    g_case = "partial 3 in 2x2";
    const Result three = LayoutEngine::compute(1920, 1080, 3);
    const Result full2 = LayoutEngine::compute(1920, 1080, 4, "2x2");
    CHECK(three.ok);
    CHECK(three.name == "2x2");
    CHECK(three.streamCount == 3);
    CHECK(three.tiles.size() == 3u);
    expectPartition(1920, 1080, full2);
    for (std::size_t i = 0; i < three.tiles.size(); ++i) {
        CHECK(sameRect(three.tiles[i].rect, full2.tiles[i].rect));
        CHECK(three.tiles[i].row == full2.tiles[i].row);
        CHECK(three.tiles[i].col == full2.tiles[i].col);
    }
    // Bottom-right quarter is the hole, not absorbed by another tile.
    CHECK(sameRect(three.tiles[0].rect, Rect{0, 0, 960, 540}));
    CHECK(sameRect(three.tiles[1].rect, Rect{960, 0, 960, 540}));
    CHECK(sameRect(three.tiles[2].rect, Rect{0, 540, 960, 540}));
    CHECK(tileArea(three.tiles) * 4 == static_cast<long long>(1920) * 1080 * 3);

    g_case = "partial 5 in 2x3";
    const Result five = LayoutEngine::compute(1920, 1080, 5);
    const Result full6 = LayoutEngine::compute(1920, 1080, 6);
    CHECK(five.ok);
    CHECK(five.name == "2x3");
    CHECK(five.tiles.size() == 5u);
    expectNoOverlap(five.tiles);
    expectInside(1920, 1080, five.tiles);
    for (std::size_t i = 0; i < five.tiles.size(); ++i) {
        CHECK(sameRect(five.tiles[i].rect, full6.tiles[i].rect));
    }
    CHECK(tileArea(five.tiles) < static_cast<long long>(1920) * 1080);

    g_case = "partial 7 and 8 in 3x3";
    const Result nine = LayoutEngine::compute(800, 600, 9);
    expectPartition(800, 600, nine);
    for (int n : {7, 8}) {
        const Result partial = LayoutEngine::compute(800, 600, n);
        CHECK(partial.ok);
        CHECK(partial.name == "3x3");
        CHECK(static_cast<int>(partial.tiles.size()) == n);
        for (std::size_t i = 0; i < partial.tiles.size(); ++i) {
            CHECK(sameRect(partial.tiles[i].rect, nine.tiles[i].rect));
        }
        expectNoOverlap(partial.tiles);
        CHECK(tileArea(partial.tiles) < tileArea(nine.tiles));
    }
}

void testAutoAndOverride() {
    const int viewports[][2] = {{1, 1}, {2, 3}, {100, 100}, {1919, 1079}, {3840, 2160}};

    for (const auto& viewport : viewports) {
        const int width = viewport[0];
        const int height = viewport[1];
        g_case = "auto 0..12";
        for (int n = 0; n <= 12; ++n) {
            const Result layout = LayoutEngine::compute(width, height, n);
            CHECK(layout.ok);
            CHECK(layout.error.empty());
            CHECK(layout.name == LayoutEngine::suggestLayout(n));
            const int capacity = layout.rows * layout.cols;
            const int expectCount = n < capacity ? n : capacity;
            CHECK(layout.streamCount == expectCount);
            CHECK(static_cast<int>(layout.tiles.size()) == expectCount);
            expectInside(width, height, layout.tiles);
            expectNoOverlap(layout.tiles);

            const Result full =
                LayoutEngine::compute(width, height, capacity, layout.name);
            expectPartition(width, height, full);
            for (std::size_t i = 0; i < layout.tiles.size(); ++i) {
                CHECK(sameRect(layout.tiles[i].rect, full.tiles[i].rect));
                CHECK(layout.tiles[i].index == static_cast<int>(i));
            }
        }

        g_case = "negative count";
        const Result negative = LayoutEngine::compute(width, height, -5);
        CHECK(negative.ok);
        CHECK(negative.name == "1x1");
        CHECK(negative.tiles.empty());
        CHECK(negative.streamCount == 0);

        g_case = "override clamps to capacity";
        const Result forced = LayoutEngine::compute(width, height, 9, "1x2");
        CHECK(forced.ok);
        CHECK(forced.name == "1x2");
        CHECK(forced.rows == 1);
        CHECK(forced.cols == 2);
        CHECK(forced.streamCount == 2);
        const Result side = LayoutEngine::compute(width, height, 2, "1x2");
        expectPartition(width, height, side);
        CHECK(sameRect(forced.tiles[0].rect, side.tiles[0].rect));
        CHECK(sameRect(forced.tiles[1].rect, side.tiles[1].rect));

        g_case = "override larger than stream count";
        const Result sparse = LayoutEngine::compute(width, height, 2, "3x3");
        const Result full9 = LayoutEngine::compute(width, height, 9, "3x3");
        CHECK(sparse.ok);
        CHECK(sparse.streamCount == 2);
        expectPartition(width, height, full9);
        CHECK(sameRect(sparse.tiles[0].rect, full9.tiles[0].rect));
        CHECK(sameRect(sparse.tiles[1].rect, full9.tiles[1].rect));
        // Two streams do not expand to fill the 3x3.
        CHECK(tileArea(sparse.tiles) < static_cast<long long>(width) * height);
    }

    g_case = "override rotations";
    const Result stacked = LayoutEngine::compute(640, 480, 2, "2x1");
    expectPartition(640, 480, stacked);
    CHECK(stacked.name == "2x1");
    CHECK(sameRect(stacked.tiles[0].rect, Rect{0, 0, 640, 240}));
    CHECK(sameRect(stacked.tiles[1].rect, Rect{0, 240, 640, 240}));

    g_case = "empty override matches omitted override";
    const Result omitted = LayoutEngine::compute(320, 240, 5);
    const Result empty = LayoutEngine::compute(320, 240, 5, "");
    CHECK(empty.ok);
    CHECK(empty.name == omitted.name);
    CHECK(empty.tiles.size() == omitted.tiles.size());
    for (std::size_t i = 0; i < empty.tiles.size(); ++i) {
        CHECK(sameRect(empty.tiles[i].rect, omitted.tiles[i].rect));
    }
}

void testRejections() {
    g_case = "bad viewport";
    expectFailure(LayoutEngine::compute(0, 1080, 4));
    expectFailure(LayoutEngine::compute(1920, 0, 4));
    expectFailure(LayoutEngine::compute(-1, 1080, 4));
    expectFailure(LayoutEngine::compute(1920, -5, 1, "1x1"));
    CHECK(LayoutEngine::compute(0, 0, 4).error.find("viewport") != std::string::npos);

    g_case = "bad override";
    const char* badNames[] = {"4x4", "2X2", "2 x 2", "nope", "3x4", "0x1", " "};
    for (const char* name : badNames) {
        const Result layout = LayoutEngine::compute(1920, 1080, 4, name);
        expectFailure(layout);
        CHECK(layout.error.find("layout") != std::string::npos);
    }

    g_case = "viewport checked before layout";
    const Result both = LayoutEngine::compute(0, 1080, 4, "nope");
    expectFailure(both);
    CHECK(both.error.find("viewport") != std::string::npos);
}

void testDegenerateViewportStillCovers() {
    g_case = "1x1 viewport 2x2";
    const Result tiny = LayoutEngine::compute(1, 1, 4, "2x2");
    expectPartition(1, 1, tiny);
    expectBitmapCover(1, 1, tiny.tiles);

    g_case = "5x4 viewport 3x3 bitmap";
    const Result small = LayoutEngine::compute(5, 4, 9, "3x3");
    expectPartition(5, 4, small);
    expectBitmapCover(5, 4, small.tiles);
}

}  // namespace

int main() {
    testSuggestLayout();
    testParseLayout();
    testHandCalculated();
    testPartialGridsDoNotStretch();
    testAutoAndOverride();
    testRejections();
    testDegenerateViewportStillCovers();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "layout_engine_tests ok\n";
    return 0;
}
