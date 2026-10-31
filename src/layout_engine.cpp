#include "layout_engine.h"

std::string LayoutEngine::suggestLayout(int streamCount) {
    if (streamCount <= 1) return "1x1";
    if (streamCount <= 2) return "1x2";
    if (streamCount <= 4) return "2x2";
    if (streamCount <= 6) return "2x3";
    if (streamCount <= 9) return "3x3";
    return "3x3";
}
