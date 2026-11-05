// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: core/ShittimUnityRandom.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file Random.h
 * @brief 复现当前 UnityPlayer 的四字全局随机状态与 Range 分支。
 */
#include <array>
#include <cstdint>

/**
 * @brief 当前 UnityPlayer.dll 的 UnityEngine.Random 算法和四字状态。
 * @details 原 native RVA：InitState 0xbc4c0、value 0xbcca0、
 * Range(float) 0xbc9d0、RandomRangeInt 0xbcab0。不是 ParticleSystem 私有 RNG。
 */
namespace baclickfx::unity {
class Random {
public:
    using State = std::array<std::uint32_t,4>;
    explicit Random(std::uint32_t seed=0) { init(seed); }
    void init(std::uint32_t seed) {
        m_state[0]=seed;
        for(int i=1;i<4;++i)m_state[i]=1812433253u*m_state[i-1]+1u;
    }
    State state() const { return m_state; }
    void setState(State state) { m_state=state; }
    std::uint32_t next() {
        const std::uint32_t t=m_state[0]^(m_state[0]<<11);
        const std::uint32_t w=m_state[3];
        m_state={m_state[1],m_state[2],w,w^(w>>19)^t^(t>>8)};
        return m_state[3];
    }
    float value() {
        return float(next()&0x7fffffu)*1.1920930376163597e-7f;
    }
    float range(float minimum,float maximum) {
        const float unit=value();
        // 原 SSE 按 value*min+(1-value)*max 逐次单精度舍入，不融合乘加。
        volatile float low=unit*minimum;
        volatile float high=(1.f-unit)*maximum;
        return high+low;
    }
    std::int32_t range(std::int32_t minimum,std::int32_t maximum) {
        if(minimum==maximum)return minimum; // 原整数等界不推进随机状态。
        if(minimum<maximum) {
            const auto span=std::uint32_t(maximum)-std::uint32_t(minimum);
            return std::int32_t(std::int64_t(minimum)+next()%span);
        }
        const auto span=std::uint32_t(minimum)-std::uint32_t(maximum);
        return std::int32_t(std::int64_t(minimum)-next()%span);
    }
private:
    State m_state{};
};

}
