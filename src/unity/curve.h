// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleCurve.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleCurve.h
 * @brief 计算源非加权曲线的系数、缓存、采样与极值。
 */
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
namespace baclickfx::unity {
// 匹配 Unity 非加权标量缓存与两段粒子缓存；当前源键均为非加权，不支持加权曲线。
template<typename Key> std::array<float,4> curveCoefficients(const Key &a,const Key &b) {
    const float out=float(a.outSlope),in=float(b.inSlope);
    if(out==std::numeric_limits<float>::infinity() || in==std::numeric_limits<float>::infinity())
        return {0,0,0,float(a.v)};
    if(out==-std::numeric_limits<float>::infinity() || in==-std::numeric_limits<float>::infinity())
        return {0,0,0,float(b.v)};
    volatile float rawTime=float(b.t)-float(a.t),delta=float(b.v)-float(a.v);
    const float dt=std::max(9.999999747378752e-5f,float(rawTime));
    volatile float inv=1.f/dt,first=out*dt,last=dt*in,square=inv*inv;
    volatile float cubic=last+first;cubic-=delta;cubic-=delta;cubic*=square;cubic*=inv;
    volatile float quad=delta+delta;quad+=delta;quad-=first;quad-=first;quad-=last;quad*=square;
    return {cubic,quad,out,float(a.v)};
}
template<typename Keys> float evaluateCurve(const Keys &keys,float time,float empty=1) {
    if(keys.empty())return empty;
    if(time<float(keys.front().t))return float(keys.front().v);
    if(time>=float(keys.back().t))return float(keys.back().v);
    size_t index=0;
    while(index+1<keys.size() && time>=float(keys[index+1].t))++index;
    const auto c=curveCoefficients(keys[index],keys[index+1]);
    volatile float offset=time-float(keys[index].t),value=offset*c[0];
    value+=c[1];value*=offset;value+=c[2];value*=offset;value+=c[3];
    return value;
}
}
namespace baclickfx::unity {
struct ParticleCurveCache {std::array<float,9> data{};bool enabled=false;};
template<typename Keys> ParticleCurveCache optimizeCurve(const Keys &keys,float multiplier) {
    ParticleCurveCache result;
    if(keys.size()>3)return result;
    if(keys.size()>1) {
        const auto &a=keys[keys.size()-2],&b=keys.back();
        volatile float lastDelta=float(a.v)-float(b.v);
        if(std::abs(float(lastDelta))>9.999999717180685e-10f && (std::isinf(float(a.outSlope))||std::isinf(float(b.inSlope))))return result;
        if(std::abs(float(keys.front().t))>9.999999747378752e-5f || std::abs(float(keys.back().t)-1.f)>9.999999747378752e-5f)return result;
    }
    result.enabled=true;result.data[8]=1;
    if(keys.empty())return result;
    if(keys.size()==1) {result.data[3]=result.data[7]=multiplier*float(keys[0].v);return result;}
    const auto first=curveCoefficients(keys[0],keys[1]);
    const auto second=keys.size()==3?curveCoefficients(keys[1],keys[2]):first;
    for(int i=0;i<4;++i) {result.data[size_t(i)]=first[size_t(i)]*multiplier;result.data[size_t(i+4)]=second[size_t(i)]*multiplier;}
    result.data[8]=keys.size()==3?float(keys[1].t):1;
    return result;
}
inline float evaluateOptimized(const ParticleCurveCache &cache,float time) {
    const auto &c=cache.data;
    const bool second=c[8]<=std::min(time,.9999899864196777f);
    volatile float offset=second?time-c[8]:time;
    const size_t index=second?4:0;
    volatile float value=c[index]*offset;value+=c[index+1];value*=offset;value+=c[index+2];value*=offset;value+=c[index+3];
    return value;
}
template<typename Keys> float evaluateParticleCurve(const Keys &keys,float time,float multiplier=1) {
    if(keys.empty())return multiplier;
    const auto cache=optimizeCurve(keys,multiplier);
    if(cache.enabled)return evaluateOptimized(cache,time);
    return evaluateCurve(keys,time)*multiplier;
}
// ad13a0 求源非加权三次段极值；端点也必须经过系数缓存，直接取末键值会改变舍入及源阶梯曲线行为。
template<typename Keys> std::array<float,2> curveExtrema(const Keys &keys,
        std::array<float,2> initial={std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
    if(keys.empty())return initial;
    auto include=[&](float value) {
#if defined(__SSE__)
        initial[0]=_mm_cvtss_f32(_mm_min_ss(_mm_set_ss(initial[0]),_mm_set_ss(value)));
        initial[1]=_mm_cvtss_f32(_mm_max_ss(_mm_set_ss(initial[1]),_mm_set_ss(value)));
#else
        initial[0]=initial[0]<value?initial[0]:value;initial[1]=initial[1]>value?initial[1]:value;
#endif
    };
    include(float(keys.front().v));
    for(std::size_t index=0;index+1<keys.size();++index) {
        const auto c=curveCoefficients(keys[index],keys[index+1]);
        auto evaluate=[&](float offset) {
            volatile float value=c[0]*offset;value+=c[1];value*=offset;value+=c[2];value*=offset;value+=c[3];
            include(value);
        };
        volatile float a=c[0]*3.f,b=c[1]*2.f;
        std::array<float,2> roots{};unsigned count=0;
        constexpr float epsilon=9.999999747378752e-6f;
        if(std::abs(float(a))<epsilon) {
            if(std::abs(float(b))>epsilon) {volatile float root=-c[2]/b;roots[0]=root;count=1;}
        } else {
            volatile float first=a*4.f;first*=c[2];
            volatile float discriminant=b*b;discriminant-=first;
            if(discriminant>=0) {
                volatile float inverse=.5f/a,radical=std::sqrt(float(discriminant));
                volatile float positive=radical-b,negative=-radical;negative-=b;
                positive*=inverse;negative*=inverse;
                roots={positive,negative};count=2;
            }
        }
        for(unsigned root=0;root<count;++root) {
            volatile float time=roots[root]+float(keys[index].t);
            if(roots[root]>=0 && time<float(keys[index+1].t))evaluate(roots[root]);
        }
        volatile float duration=float(keys[index+1].t)-float(keys[index].t);
        evaluate(duration);
    }
    return initial;
}
// ad5da0 使用包含零的 Constant 范围、排序后的 TwoConstants 范围，或原曲线极值乘标量；源函数对负标量乘积不重新排列端点。
template<typename Keys> std::array<float,2> particleCurveExtrema(int mode,float low,float high,
                                                             const Keys &maximumCurve,const Keys &minimumCurve) {
    if(mode==0)return high>0?std::array<float,2>{0,high}:std::array<float,2>{high,0};
    if(mode==3)return high>low?std::array<float,2>{low,high}:std::array<float,2>{high,low};
    auto extrema=curveExtrema(maximumCurve);
    if(mode==2)extrema=curveExtrema(minimumCurve,extrema);
    for(auto &value:extrema) {volatile float scaled=high*value;value=scaled;}
    return extrema;
}
}
