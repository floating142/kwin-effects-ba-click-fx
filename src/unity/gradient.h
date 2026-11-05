// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleGradient.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleGradient.h
 * @brief 按源 uint16 时间键和 RGBA8 规则插值粒子颜色。
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace baclickfx::unity {
inline std::uint8_t colorByte(float value) {
    volatile float scaled = std::clamp(value, 0.f, 1.f) * 255.f;
    return std::uint8_t(scaled + .5f);
}

struct ColorKey {
    std::uint16_t time=0;
    std::array<float,3> value{1,1,1};
};
struct AlphaKey {
    std::uint16_t time=0;
    float value=1;
};

// 源 GradientMode.Blend SIMD 运算，按单 lane 求值；保留序列化 uint16 时间，不能用归一化 double 键替换边界上的 float32 减除。
struct Gradient {
    bool enabled=false;
    std::vector<ColorKey> colors;
    std::vector<AlphaKey> alphas;
    std::vector<float> times,inverses;
    std::vector<std::array<std::uint8_t,4>> cache;
    template<typename Keys> static float weight(const Keys &keys,float time,size_t &index) {
        const float bounded=std::clamp(time,float(keys.front().time),float(keys.back().time));
        index=1;
        while(index+1<keys.size() && float(keys[index].time)<bounded)++index;
        volatile float numerator=bounded-float(keys[index-1].time);
        volatile float denominator=std::max(float(keys[index].time)-float(keys[index-1].time),1e-6f);
        return std::min(numerator/denominator,1.f);
    }
    static float blend(float a,float b,float unit) {
        volatile float difference=b-a;
        volatile float product=difference*unit;
        return a+product;
    }
    std::array<float,4> evaluate(float progress) const {
        std::array<float,4> out{1,1,1,1};
        if(!enabled)return out;
        volatile float time=progress*65535.f;
        size_t index=0;
        if(colors.size()>1) {
            const float unit=weight(colors,time,index);
            for(size_t ch=0;ch<3;++ch)out[ch]=blend(colors[index-1].value[ch],colors[index].value[ch],unit);
        }
        if(alphas.size()>1) {
            const float unit=weight(alphas,time,index);
            out[3]=blend(alphas[index-1].value,alphas[index].value,unit);
        }
        return out;
    }
    std::array<std::uint8_t,4> evaluateBytes(float progress) const {
        const auto value=evaluate(progress);
        return {colorByte(value[0]),colorByte(value[1]),colorByte(value[2]),colorByte(value[3])};
    }
    static float inverseInterval(float value) {
#if defined(__SSE__)
        float estimate=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(value)));
        for(int i=0;i<2;++i) {
            volatile float product=estimate*value;
            volatile float correction=(i==0?2.000000476837158203125f:2.f)-product;
            estimate=correction*estimate;
        }
        return estimate;
#else
        return 1.f/value;
#endif
    }
    void prepareCache() {
        times.clear();inverses.clear();cache.clear();
        if(!enabled)return;
        // 源缓存生成器对 RGB 时间乘倒数，对 alpha 时间直接相除；两者可能产生不同位值，须保留。
        for(const auto &key:colors)times.push_back(float(key.time)*(1.f/65535.f));
        for(const auto &key:alphas)times.push_back(float(key.time)/65535.f);
        std::sort(times.begin(),times.end());
        times.erase(std::unique(times.begin(),times.end()),times.end());
        if(times.size()<16)times.push_back(1.f);else times.back()=1.f;
        for(float time:times)cache.push_back(evaluateBytes(time));
        inverses.resize(times.size());
        for(size_t i=1;i<times.size();++i)
            inverses[i]=inverseInterval(std::max(times[i]-times[i-1],1e-6f));
    }
    std::array<std::uint8_t,4> sampleCached(float progress) const {
        std::array<std::uint8_t,4> out{255,255,255,255};
        if(!enabled || times.size()<2)return out;
        size_t index=1;
        while(index<times.size() && times[index]<progress)++index;
        if(index==times.size())return out;
        // 相等时进入下一段，源 SIMD 完成 lane mask 为 progress < nextTime，非 <=。
        do {
            volatile float delta=progress-times[index-1];
            volatile float unit=std::clamp(float(delta),0.f,1.f)*inverses[index];
            volatile float scaled=unit*255.f;
            const int weight=int(scaled);
            for(size_t ch=0;ch<4;++ch) {
                const int product=(int(cache[index][ch])-int(cache[index-1][ch]))*weight+128;
                const int step=product>=0?product/256:-((-product+255)/256);
                out[ch]=std::uint8_t((int(cache[index-1][ch])+step)&255);
            }
            if(progress<times[index])break;
        } while(++index<times.size());
        return out;
    }
};
// ColorOverLifetime 四 lane 更新将整数乘积除以 255 后舍入；标量 sub-emitter 辅助函数公式不同，此处不用。
inline std::uint8_t multiplyByte(std::uint8_t start,std::uint8_t gradient) {
    const unsigned product=unsigned(start)*unsigned(gradient)+128u;
    return std::uint8_t((product+(product>>8))>>8);
}
inline std::array<std::uint8_t,4> lifetimeColor(const std::array<std::uint8_t,4> &start,
                                               const Gradient &gradient,float progress) {
    if(!gradient.enabled)return start;
    const auto color=gradient.sampleCached(progress);
    return {multiplyByte(start[0],color[0]),multiplyByte(start[1],color[1]),
            multiplyByte(start[2],color[2]),multiplyByte(start[3],color[3])};
}
}
