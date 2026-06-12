#pragma once

// Pure YOLOX geometry helpers shared by detector_person.cpp and the unit
// tests. Header-only on purpose: no onnxruntime, no windows.h.

#include "roi_geometry.h"

#include <algorithm>
#include <vector>

struct GridStride {
    int grid_x;
    int grid_y;
    int stride;
};

inline void GenerateGridStrides(int input_w, int input_h, std::vector<GridStride>& out) {
    out.clear();
    const int strides[3] = { 8, 16, 32 };
    for (int s : strides) {
        const int gh = input_h / s;
        const int gw = input_w / s;
        for (int gy = 0; gy < gh; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                out.push_back({ gx, gy, s });
            }
        }
    }
}

struct Letterbox {
    float scale;
    int pad_x;
    int pad_y;
    int new_w;
    int new_h;
};

inline Letterbox ComputeLetterbox(int orig_w, int orig_h, int input_w, int input_h) {
    Letterbox lb;
    const float sw = static_cast<float>(input_w) / orig_w;
    const float sh = static_cast<float>(input_h) / orig_h;
    lb.scale = (sw < sh) ? sw : sh;
    lb.new_w = static_cast<int>(orig_w * lb.scale);
    lb.new_h = static_cast<int>(orig_h * lb.scale);
    lb.pad_x = (input_w - lb.new_w) / 2;
    lb.pad_y = (input_h - lb.new_h) / 2;
    return lb;
}

struct ScoredBox {
    DetectionRect box;
    float score;
};

inline float Iou(const DetectionRect& a, const DetectionRect& b) {
    const int x1 = (a.x > b.x) ? a.x : b.x;
    const int y1 = (a.y > b.y) ? a.y : b.y;
    const int ax2 = a.x + a.w;
    const int ay2 = a.y + a.h;
    const int bx2 = b.x + b.w;
    const int by2 = b.y + b.h;
    const int x2 = (ax2 < bx2) ? ax2 : bx2;
    const int y2 = (ay2 < by2) ? ay2 : by2;
    const int iw = x2 - x1;
    const int ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.f;
    const int inter = iw * ih;
    const int uni = a.w * a.h + b.w * b.h - inter;
    return (uni > 0) ? static_cast<float>(inter) / static_cast<float>(uni) : 0.f;
}

inline void Nms(std::vector<ScoredBox>& boxes, float iou_thresh,
                std::vector<DetectionRect>& kept) {
    std::sort(boxes.begin(), boxes.end(),
              [](const ScoredBox& a, const ScoredBox& b) { return a.score > b.score; });
    std::vector<char> suppressed(boxes.size(), 0);
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(boxes[i].box);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;
            if (Iou(boxes[i].box, boxes[j].box) > iou_thresh) {
                suppressed[j] = 1;
            }
        }
    }
}
