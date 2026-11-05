// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: fx/particles/ShittimParticleInitial.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleInitial.h
 * @brief 采样源 InitialModule 的四 lane 随机起始属性。
 */
#include "random.h"
#include "gradient.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

// 当前 UnityPlayer 的 InitialModule 四 lane RNG 和起始属性采样。
// 不是全局 UnityEngine.Random；形状／发射步进另行处理。
namespace baclickfx::unity {
// InitialModule 保存 reciprocal(max(1e-5, sampledLifetime))；保留源 rcpps 估计、带偏置首轮细化和第二轮细化。
inline float inverseLifetime(float lifetime,float minimum=9.999999747378752e-6f) {
    const float value=std::max(minimum,lifetime);
#if defined(__SSE__)
    const float initial=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(value)));
    volatile float product=initial*value,correction=2.000000476837158f-product;
    volatile float first=correction*initial,secondProduct=first*value;
    volatile float secondCorrection=2.f-secondProduct;
    const float result=first*secondCorrection;
    return std::isnan(result)?initial:result;
#else
    return 1.f/value;
#endif
}
struct Range {
    float low=0, high=0;
    int mode=0; // ParticleSystemCurveMode.Constant=0, TwoConstants=3
    float sample(float unit) const {
        if(mode==0)return high;
        volatile float difference=high-low;
        volatile float product=difference*unit;
        return low+product;
    }
};
struct Config {
    Range lifetime,sizeX,sizeY,sizeZ,rotationX,rotationY,rotationZ;
    bool size3D=false,rotation3D=false;
    float flipRotation=0;
    int colorMode=0;
    std::array<float,4> colorLow{1,1,1,1},colorHigh{1,1,1,1};
};
struct Sample {
    std::uint32_t seed=0;
    float lifetime=0;
    float inverseLifetime=0;
    std::array<float,3> size{},rotation{};
    std::array<std::uint8_t,4> color{};
};
class Initial {
public:
    explicit Initial(std::uint32_t seed=0) { init(seed); }
    void init(std::uint32_t seed) {
        for(std::uint32_t lane=0;lane<4;++lane)m_streams[lane].init(seed+367u*lane);
        m_cursor=4;
    }
    std::array<std::uint32_t,16> state() const {
        std::array<std::uint32_t,16> result{};
        for(int lane=0;lane<4;++lane)for(int word=0;word<4;++word)
            result[size_t(word*4+lane)]=m_streams[size_t(lane)].state()[size_t(word)];
        return result;
    }
    void discardBatch() { m_cursor=4; }
    void setState(const std::array<std::uint32_t,16> &state) {
        for(int lane=0;lane<4;++lane) {
            Random::State words{};
            for(int word=0;word<4;++word)words[size_t(word)]=state[size_t(word*4+lane)];
            m_streams[size_t(lane)].setState(words);
        }
        discardBatch();
    }
    Sample next(const Config &c) {
        if(m_cursor==4)batch(c);
        return m_samples[size_t(m_cursor++)];
    }
private:
    void batch(const Config &c) {
        for(int lane=0;lane<4;++lane) {
            auto &rng=m_streams[size_t(lane)];auto &p=m_samples[size_t(lane)];
            p.seed=rng.next();
            Random flip(p.seed+0xff2bb1a4u);
            const float sign=c.flipRotation<flip.value()?1.f:-1.f;
            p.lifetime=c.lifetime.sample(rng.value());
            p.inverseLifetime=baclickfx::unity::inverseLifetime(p.lifetime);
            p.size[0]=std::max(0.f,c.sizeX.sample(rng.value()));
            p.size[1]=c.size3D?std::max(0.f,c.sizeY.sample(rng.value())):p.size[0];
            p.size[2]=c.size3D?std::max(0.f,c.sizeZ.sample(rng.value())):p.size[0];
            p.rotation[2]=c.rotationZ.sample(rng.value())*sign;
            p.rotation[0]=c.rotation3D?c.rotationX.sample(rng.value())*sign:0;
            p.rotation[1]=c.rotation3D?c.rotationY.sample(rng.value())*sign:0;
            const float mix=rng.value();
            for(int ch=0;ch<4;++ch) {
                const Range channel{c.colorLow[size_t(ch)],c.colorHigh[size_t(ch)],c.colorMode==2?3:0};
                p.color[size_t(ch)]=colorByte(channel.sample(mix));
            }
        }
        m_cursor=0;
    }
    std::array<Random,4> m_streams;
    std::array<Sample,4> m_samples{};
    int m_cursor=4;
};
inline float startDelay(std::uint32_t seed,const Range &range) {
    Random rng(seed);
    return range.sample(rng.value()); // 独立 seed 的首个 draw，不消耗 InitialModule。
}
inline float radialSpeed(std::uint32_t particleSeed,const Range &range) {
    Random rng(particleSeed+0xcab3921du);
    return range.sample(rng.value());
}
inline float startSpeed(std::uint32_t particleSeed,const Range &range) {
    Random rng(particleSeed+0x96aa4de3u);
    return range.sample(rng.value());
}
}
