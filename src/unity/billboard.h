// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleSize.h.
#pragma once

/**
 * @file ShittimParticleSize.h
 * @brief 按源相机与粒子参数计算尺寸和投影边界。
 */
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace baclickfx::unity {
// e9668b..e9677d 先将相机平面应用到世界位置，再合并透视和正交边界；保留源 float32 加法顺序。
inline std::array<float,2> particleBillboardDepthLimits(const std::array<float,4> &limits,
        const std::array<float,4> &plane,const std::array<float,3> &position)
{
    // 即使未启用 fast-math，C++ 也允许交换可交换操作数，可能改变所选 NaN 的符号与载荷；须保留源目标操作数。
#if defined(__SSE2__) && (defined(__GNUC__) || defined(__clang__))
    const auto mul=[](float a,float b){asm("mulss %1,%0":"+x"(a):"x"(b));return a;};
    const auto add=[](float a,float b){asm("addss %1,%0":"+x"(a):"x"(b));return a;};
#else
    const auto mul=[](float a,float b){volatile float r=a*b;return r;};
    const auto add=[](float a,float b){volatile float r=a+b;return r;};
#endif
    const float depth=add(plane[3],add(add(mul(plane[0],position[0]),mul(plane[1],position[1])),mul(plane[2],position[2])));
    return {add(mul(limits[0],depth),limits[2]),add(mul(limits[1],depth),limits[3])};
}
// ac54cb..ac55e3 在逐粒子深度和半尺寸计算前打包透视与正交限制；相机和有效缩放由调用方准备，保留原除乘顺序。
inline std::array<float,4> particleBillboardLimits(float minimum,float maximum,
        float contextScaleX,float horizontalSpan,float farClip,bool orthographic)
{
    const float scale=1e-5f>contextScaleX?1e-5f:contextScaleX;
    std::array<float,4> result{};
#if defined(__SSE2__)
    const auto source=_mm_set_ps(0,0,maximum,minimum);
    const auto divided=_mm_div_ps(source,_mm_set1_ps(scale));
    auto packed=_mm_mul_ps(divided,_mm_set1_ps(orthographic?horizontalSpan:-horizontalSpan));
    if(!orthographic)packed=_mm_div_ps(packed,_mm_set1_ps(farClip));
    alignas(16) std::array<float,4> values;
    _mm_store_ps(values.data(),packed);
#else
    std::array<float,4> values{};
    for(unsigned axis=0;axis<2;++axis) {
        volatile float divided=(axis?maximum:minimum)/scale;
        volatile float packed=divided*(orthographic?horizontalSpan:-horizontalSpan);
        values[axis]=orthographic?float(packed):float(packed)/farClip;
    }
#endif
    for(unsigned axis=0;axis<2;++axis) {
        uint32_t bits;
        std::memcpy(&bits,&values[axis],sizeof(bits));
        result[(orthographic?2:0)+axis]=(bits&0x7f800000u)==0x7f800000u
                ?(axis?-1.f:0.f):values[axis];
    }
    return result;
}
// UnityPlayer e9694d/e9270d 在组合父轴前裁限 billboard 尺寸；限制已除以 context X scale。
// 负 maximum 关闭上界，负 minimum 清除下界结果；寿命 mask 判断百分数年龄 < 100，非 < 1。
inline std::array<float,2> particleBillboardHalfExtents(float x,float y,
        float minimum,float maximum,float percentAge=0)
{
#if defined(__SSE2__)
    const auto zero=_mm_setzero_ps(),half=_mm_set1_ps(.5f);
    const auto sx=_mm_set1_ps(x),sy=_mm_set1_ps(y);
    const auto lo=_mm_set1_ps(minimum),hi=_mm_set1_ps(maximum);
    const auto largest=_mm_max_ps(_mm_max_ps(sx,sy),_mm_set1_ps(1e-6f));
    const auto lower=_mm_and_ps(_mm_mul_ps(_mm_max_ps(largest,lo),half),_mm_cmple_ps(zero,lo));
    const auto upperMask=_mm_cmple_ps(zero,hi);
    const auto bounded=_mm_or_ps(_mm_and_ps(_mm_min_ps(lower,_mm_mul_ps(half,hi)),upperMask),
                                 _mm_andnot_ps(upperMask,lower));
    const auto ratio=_mm_div_ps(bounded,largest);
    const auto alive=_mm_cmplt_ps(_mm_set1_ps(percentAge),_mm_set1_ps(100.f));
    return {_mm_cvtss_f32(_mm_and_ps(_mm_mul_ps(sx,ratio),alive)),
            _mm_cvtss_f32(_mm_and_ps(_mm_mul_ps(ratio,sy),alive))};
#else
    const float largest=std::max(std::max(x,y),1e-6f);
    volatile float lower=minimum>=0?std::max(largest,minimum)*.5f:0.f;
    volatile float upper=.5f*maximum;
    volatile float ratio=(maximum>=0?std::min(float(lower),float(upper)):float(lower))/largest;
    if(!(percentAge<100))return {0,0};
    return {x*ratio,ratio*y};
#endif
}
}
