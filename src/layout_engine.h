#pragma once
#include <string>

class LayoutEngine {
public:
    // Returns suggested grid, e.g. "2x2" for 4 streams
    static std::string suggestLayout(int streamCount);
};
