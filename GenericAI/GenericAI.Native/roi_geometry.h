#pragma once

#include <algorithm>

struct DetectionRect {
    int x;
    int y;
    int w;
    int h;
};

struct ROIRect {
    int x1;
    int y1;
    int x2;
    int y2;
};

inline bool BoxOverlapsRoi(const DetectionRect& b, const ROIRect& r) {
    const int rx1 = std::min(r.x1, r.x2);
    const int rx2 = std::max(r.x1, r.x2);
    const int ry1 = std::min(r.y1, r.y2);
    const int ry2 = std::max(r.y1, r.y2);
    const int bx1 = b.x;
    const int by1 = b.y;
    const int bx2 = b.x + b.w;
    const int by2 = b.y + b.h;
    return !(bx2 <= rx1 || bx1 >= rx2 || by2 <= ry1 || by1 >= ry2);
}
