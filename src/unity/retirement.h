// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleRetirement.h.
// Native reference: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleRetirement.h
 * @brief 按源四 lane 扫描及末尾覆盖规则回收死亡粒子。
 */
#include <cstddef>
namespace baclickfx::unity {
inline bool survives(float percentAge) { return !(percentAge>100.f); }
// 源单 job 四 lane 死亡扫描；调用方提供 SoA 访问器和完整末粒子复制。年龄等于 100 时仍存活。
template<class Age,class Copy>
std::size_t compactDead(std::size_t count,Age age,Copy copy) {
    std::size_t block=0;
    while(block<count) {
        unsigned mask=0;
        for(unsigned lane=0;lane<4 && block+lane<count;++lane)
            if(age(block+lane)>100.f)mask|=1u<<lane;
        if(mask) {
            for(int lane=3;lane>=0;--lane)
                if(mask&(1u<<unsigned(lane))) {copy(block+std::size_t(lane),count-1);--count;}
        } else block+=4;
    }
    return count;
}
}
