// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: fx/particles/ShittimParticleShape.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleShape.h
 * @brief 按源 Box 发射器规则采样并归一化方向。
 */
#include "random.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace baclickfx::unity {
using ShapeVector=std::array<float,3>;
// 保留源 SIMD 归一化常量和运算顺序；SSE 路径与当前游戏 UnityPlayer 对照，其他架构回退到 sqrt。
inline ShapeVector shapeNormalize(const ShapeVector &v,bool transformed=false) {
    volatile float xx=v[0]*v[0],yy=v[1]*v[1],zz=v[2]*v[2];
    volatile float tail=transformed?zz+yy:yy+zz;
    const float length=transformed?tail+xx:xx+tail;
    if(!(length>1.0000000031710769e-30f))return {0,0,1};
#if defined(__SSE__)
    volatile float estimate=_mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(length)))*0.9999998211860657f;
    const float inv=std::min(float(estimate),std::numeric_limits<float>::max());
    volatile float first=inv*length,second=first*inv;
    volatile float residual=3.000000238418579f-second,half=.5f*inv;
    const float factor=residual*half;
#else
    const float factor=1.f/std::sqrt(length);
#endif
    return {factor*v[0],factor*v[1],factor*v[2]};
}
struct ShapeSample { ShapeVector position{},direction{}; };
}
