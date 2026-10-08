#pragma once

#include "gai_abi.h"   // GAI_Roi

#include <algorithm>
#include <climits>
#include <vector>

struct DetectionRect {
    int x;
    int y;
    int w;
    int h;
    // class_table.h supported-class index; set explicitly by the detector
    // (PostYolox). NO default member initializer — that would make DetectionRect a
    // non-aggregate and break the brace-init {x,y,w,h} used in tests / motion.
    // Brace-init with 4 values value-initialises cls and score to 0.
    int cls;
    // Detection confidence (0..1). Carried through NMS so per-ROI confidence
    // filtering can run after NMS in ApplyRoiFilter. Also no default initializer.
    float score;
};

struct ROIRect {
    int x1;
    int y1;
    int x2;
    int y2;
    // Per-ROI motion tuning (schema scope=roi): each detection region carries its
    // own sensitivity/threshold so regions can be tuned independently. The motion
    // detector reads these per ROI; object detection ignores them. Defaults mirror
    // the motion schema (sensitivity=50, threshold=25) so a rect built without an
    // explicit value still behaves. No positional brace-init of ROIRect exists, so
    // the default member initializers are safe (do not break aggregate use).
    int sensitivity = 50;
    int threshold = 25;
    // Per-ROI object-detection tuning (schema scope=roi). -1 = unset -> inherit the
    // channel ai_settings value (resolved in ApplyRoiFilter). object_size_* are % of
    // frame area (0..100). class_mask is a class_table.h supported-index bitmask.
    float confidence = -1.0f;
    int   class_mask = -1;
    float object_size_min = -1.0f;
    float object_size_max = -1.0f;

    // The default member initializers above make ROIRect a non-aggregate, so the
    // 4-value brace-init used across the code/tests (ROIRect{x1,y1,x2,y2}) needs an
    // explicit constructor. This one sets only the geometry; the tuning fields keep
    // their "unset" defaults (motion 50/25, object-detection -1 = inherit channel).
    ROIRect() = default;
    ROIRect(int a, int b, int c, int d) : x1(a), y1(b), x2(c), y2(d) {}
};

// ROI coordinates arrive in the /SetParameters reference space
// (image_width x image_height) while frames carry their own resolution; these
// helpers map the former onto the latter. Scaling is skipped when either size
// is unknown (<= 0, legacy caller) or the two spaces already match.
inline bool RoiScaleNeeded(int cfg_w, int cfg_h, int frame_w, int frame_h) {
    if (cfg_w <= 0 || cfg_h <= 0 || frame_w <= 0 || frame_h <= 0) return false;
    return cfg_w != frame_w || cfg_h != frame_h;
}

inline int ScaleCoordToFrame(int v, int cfg, int frame) {
    return static_cast<int>(v * static_cast<double>(frame) / cfg + 0.5);
}

inline void ScaleRoiRectToFrame(ROIRect& r, int cfg_w, int cfg_h,
                                int frame_w, int frame_h) {
    if (!RoiScaleNeeded(cfg_w, cfg_h, frame_w, frame_h)) return;
    r.x1 = ScaleCoordToFrame(r.x1, cfg_w, frame_w);
    r.y1 = ScaleCoordToFrame(r.y1, cfg_h, frame_h);
    r.x2 = ScaleCoordToFrame(r.x2, cfg_w, frame_w);
    r.y2 = ScaleCoordToFrame(r.y2, cfg_h, frame_h);
}

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

// ---- Polygon helpers (shared by motion + object detection) ------------------
// ROI polygons arrive as up to 10 vertices in frame coordinates (scaled by the
// scheduler). These honor the actual ROI shape instead of just its bbox.

// Even-odd (ray-casting) point-in-polygon. int->double on the cross term avoids
// overflow at 4K coordinates.
inline bool PointInPolygon(int px, int py, const std::vector<GAI_Roi>& poly) {
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const int xi = poly[i].x, yi = poly[i].y;
        const int xj = poly[j].x, yj = poly[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < static_cast<double>(xj - xi) * (py - yi) / static_cast<double>(yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// Shoelace absolute area (px^2) — lets a trigger's min-area baseline track the
// polygon's REAL area, not its (larger) bounding box.
inline double PolygonArea(const std::vector<GAI_Roi>& poly) {
    double a = 0.0;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        a += static_cast<double>(poly[j].x + poly[i].x) * (poly[j].y - poly[i].y);
    }
    return std::abs(a) * 0.5;
}

// Do segments p1p2 and p3p4 intersect? Orientation test in 64-bit to avoid
// overflow on the cross products.
inline bool SegmentsIntersect(int x1, int y1, int x2, int y2,
                              int x3, int y3, int x4, int y4) {
    auto cross = [](long long ax, long long ay, long long bx, long long by) {
        return ax * by - ay * bx;
    };
    const long long d1 = cross(x2 - x1, y2 - y1, x3 - x1, y3 - y1);
    const long long d2 = cross(x2 - x1, y2 - y1, x4 - x1, y4 - y1);
    const long long d3 = cross(x4 - x3, y4 - y3, x1 - x3, y1 - y3);
    const long long d4 = cross(x4 - x3, y4 - y3, x2 - x3, y2 - y3);
    if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) return true;
    return false;   // collinear/touching cases are treated as non-crossing;
                    // containment is covered by the corner/vertex tests below.
}

// True if detection box b overlaps ROI polygon (faithful "any overlap", matching
// BoxOverlapsRoi's rectangle semantics). Falls back to false for <3 vertices —
// callers use BoxOverlapsRoi(rect) in that degenerate case.
inline bool BoxOverlapsPolygon(const DetectionRect& b, const std::vector<GAI_Roi>& poly) {
    const size_t n = poly.size();
    if (n < 3) return false;

    const int bx1 = b.x, by1 = b.y, bx2 = b.x + b.w, by2 = b.y + b.h;

    // Quick reject via polygon bbox vs box bbox.
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (const auto& p : poly) {
        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
    }
    if (bx2 <= minx || bx1 >= maxx || by2 <= miny || by1 >= maxy) return false;

    // Any box corner inside the polygon.
    if (PointInPolygon(bx1, by1, poly) || PointInPolygon(bx2, by1, poly) ||
        PointInPolygon(bx1, by2, poly) || PointInPolygon(bx2, by2, poly)) return true;

    // Any polygon vertex inside the box (polygon smaller than / contained in box).
    for (const auto& p : poly) {
        if (p.x >= bx1 && p.x < bx2 && p.y >= by1 && p.y < by2) return true;
    }

    // Any polygon edge crossing any box edge (overlap with no vertex containment).
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const int ex1 = poly[j].x, ey1 = poly[j].y, ex2 = poly[i].x, ey2 = poly[i].y;
        if (SegmentsIntersect(ex1, ey1, ex2, ey2, bx1, by1, bx2, by1) ||  // top
            SegmentsIntersect(ex1, ey1, ex2, ey2, bx2, by1, bx2, by2) ||  // right
            SegmentsIntersect(ex1, ey1, ex2, ey2, bx2, by2, bx1, by2) ||  // bottom
            SegmentsIntersect(ex1, ey1, ex2, ey2, bx1, by2, bx1, by1))    // left
            return true;
    }
    return false;
}
