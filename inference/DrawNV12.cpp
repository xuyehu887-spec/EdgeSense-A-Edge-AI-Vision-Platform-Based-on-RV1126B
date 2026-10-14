#include "DrawNV12.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {
float clip(float v, float hi) { return std::max(0.0f, std::min(v, hi)); }
void greenRect(VideoFrame& f, int l, int t, int r, int b) {
    if (l >= r || t >= b) return;
    uint8_t* uv = f.data + static_cast<size_t>(f.width) * f.height;
    for (int y = t; y < b; ++y) {
        uint8_t* row = f.data + static_cast<size_t>(y) * f.width;
        std::fill(row + l, row + r, static_cast<uint8_t>(145));
    }
    const int uvRight = (r + 1) & ~1;
    for (int y = t / 2; y < (b + 1) / 2; ++y) {
        uint8_t* row = uv + static_cast<size_t>(y) * f.width;
        for (int x = l & ~1; x < uvRight; x += 2) {
            row[x] = 54; row[x + 1] = 34;
        }
    }
}
}
void drawDetectionsNV12(VideoFrame& f,
                        const std::vector<Detection>& detections,
                        int thickness) {
    if (!f.data || f.format != PIXEL_FORMAT_NV12 || f.width <= 0 ||
        f.height <= 0 || (f.width & 1) || (f.height & 1) ||
        f.size <= 0 || thickness <= 0) return;
    const uint64_t required = static_cast<uint64_t>(f.width) * f.height * 3 / 2;
    if (required > static_cast<uint64_t>(f.size)) return;
    for (const Detection& d : detections) {
        if (!std::isfinite(d.x1) || !std::isfinite(d.y1) ||
            !std::isfinite(d.x2) || !std::isfinite(d.y2)) continue;
        const float x1 = clip(d.x1, static_cast<float>(f.width));
        const float y1 = clip(d.y1, static_cast<float>(f.height));
        const float x2 = clip(d.x2, static_cast<float>(f.width));
        const float y2 = clip(d.y2, static_cast<float>(f.height));
        if (x2 <= x1 || y2 <= y1) continue;
        const int l = static_cast<int>(std::floor(x1));
        const int t = static_cast<int>(std::floor(y1));
        const int r = static_cast<int>(std::ceil(x2));
        const int b = static_cast<int>(std::ceil(y2));
        const int k = std::min(thickness, std::min(r-l, b-t));
        greenRect(f, l, t, r, t+k);
        greenRect(f, l, b-k, r, b);
        greenRect(f, l, t, l+k, b);
        greenRect(f, r-k, t, r, b);
    }
}
