#pragma once

struct ROIRect {
    int x1;
    int y1;
    int x2;
    int y2;
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
