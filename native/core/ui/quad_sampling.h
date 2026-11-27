// Pure quad-compression helper deciding whether Canvas::ImageQuad needs multi-sample filtering.
#pragma once

#include <d2d1.h>

#include <algorithm>
#include <cmath>

namespace atrium {

inline bool NeedsMultiSampleQuad(float srcW, float srcH, const D2D1_POINT_2F quad[4])
{
    if (!quad || !std::isfinite(srcW) || !std::isfinite(srcH) || srcW <= 0.0f || srcH <= 0.0f)
        return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(quad[i].x) || !std::isfinite(quad[i].y))
            return false;
    const float topW = std::hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y);
    const float botW = std::hypot(quad[2].x - quad[3].x, quad[2].y - quad[3].y);
    const float leftH = std::hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y);
    const float rightH = std::hypot(quad[2].x - quad[1].x, quad[2].y - quad[1].y);
    const float minW = std::min(topW, botW);
    const float minH = std::min(leftH, rightH);
    if (!std::isfinite(minW) || !std::isfinite(minH))
        return false;
    return srcW > minW * 4.0f || srcH > minH * 4.0f;
}

} // namespace atrium
