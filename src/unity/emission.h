// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleEmission.h.
#pragma once

/**
 * @file ShittimParticleEmission.h
 * @brief 保留源单 job 发射窗口、余量及出生批次状态。
 */
#include "random.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <cmath>

namespace baclickfx::unity {
struct EmissionBurst {
    float time=0;
    int count=0,cycles=1;
    float interval=.01f;
};
struct ConstantEmission {
    float rate=0;
    std::vector<EmissionBurst> bursts;
};
struct EmissionState {
    float intervalScale=0,carry=0;
    Random random;
    void init(std::uint32_t seed) {intervalScale=carry=0;random.init(seed);}
};
struct EmissionAmount {std::uint64_t rate=0,total=0;};
struct SourceSchedule {
    float remaining=0,phase=0,delay=0;
    std::uint32_t loops=0;
    bool stopped=false;
};
// 单 job ae07f0 / afc440 调度，剩余时间由调用方提供；不实现外层 Unity PlayerLoop 或 job 分派。
template<class Existing,class Emit>
void advanceSourceSchedule(SourceSchedule &state,float initialStep,float duration,bool looping,
                           bool fixedStep,bool wholeSteps,Existing existing,Emit births) {
    if(initialStep<=0 || duration<=0)return;
    const bool first=state.phase==0;
    const float minimum=wholeSteps?initialStep:9.999999974752427e-7f;
    float dt=initialStep;
    while(state.remaining>=minimum) {
        const float next=std::min(state.remaining,initialStep);
        if(!fixedStep && state.remaining>10.f) {
            if(dt<=1.f)dt=std::min(duration,1.f);
        } else if(!fixedStep && state.remaining>5.f) {
            if(dt<=.20000000298023224f)dt=std::min(duration,.20000000298023224f);
        } else dt=next;
        if(dt<=0)return;
        float begin=std::min(state.phase,duration);
        if(duration<=dt)begin+=9.999999974752427e-7f;
        if(dt>state.delay) {
            volatile float active=dt-state.delay;
            state.phase+=active;
            if(looping)while(state.phase>=duration) {state.phase-=duration;++state.loops;}
            else state.phase=std::min(state.phase,duration);
        }
        volatile float ratio=state.remaining/dt,backlog=ratio-1.f;
        if(!looping && state.phase>=duration)state.stopped=true;
        existing(dt);
        if(!state.stopped) {
            float active=dt;
            if(first && state.delay>0) {
                volatile float delay=state.delay-dt;
                active=std::max(0.f,-float(delay));state.delay=std::max(0.f,float(delay));
            }
            if(active>0)births(begin,state.phase,active,std::max(0.f,float(backlog)));
        }
        state.remaining-=dt;
    }
}

// 当前源使用 Constant rate/count，概率为 1；这里只复现模块计数运算，不负责出生调度或 RNG 初始化。
inline int countBursts(const ConstantEmission &module,float begin,float end,bool repeats) {
    int count=0;
    for(const auto &b:module.bursts) {
        bool hit=b.time>=begin && b.time<end;
        if(!hit && repeats && b.cycles!=1 && begin>b.time) {
            volatile float previous=(begin-b.time)/b.interval;
            if(b.cycles==0 || float(b.cycles-1)>previous) {
                volatile float current=(end-b.time)/b.interval;
                hit=int(current)>int(previous);
            }
        }
        if(hit)count+=b.count;
    }
    return count;
}
// emitBatch 为每个 lane 计算年龄比例，同一局部 dt 同时用于出生链和首次位置、百分数年龄积分；被 mask 的 Burst 比例裁到 1e-6，填充 lane 仍消耗 RNG。
inline float birthFraction(std::uint64_t lane,std::uint64_t rateCount,const EmissionState &state) {
    volatile float index=float(std::int32_t(lane)),sum=index+state.carry;
    volatile float fraction=sum*state.intervalScale;
    return std::clamp(lane<rateCount?float(fraction):0.f,9.999999974752427e-7f,1.f);
}
inline float birthTimestep(std::uint64_t lane,std::uint64_t rateCount,const EmissionState &state,
                           float step,float offset=0) {
    volatile float partial=birthFraction(lane,rateCount,state)*step;
    return partial-offset;
}
inline std::size_t birthStorageSource(std::size_t slot,std::size_t accepted,std::size_t previous) {
    const std::size_t gap=((previous+3)&~std::size_t(3))-previous;
    const std::size_t moved=std::min(gap,accepted);
    return slot<moved?accepted-moved+slot:slot-moved;
}
// emitBatch 正向扫描全部已初始化 SIMD lane，包括填充；删除 lane 时用当前尾部覆盖，接受出生数随之饱和递减。
// finishBirthCapacity 再从剩余出生尾部填充旧对齐间隙并丢弃 padding。
// 与 compactDead 的既有粒子四 lane 反向扫描分开保留。
template<class Age,class Copy>
std::size_t finishBirthStorage(std::size_t previous,std::size_t accepted,Age age,Copy copy) {
    if(!accepted)return previous;
    const std::size_t first=(previous+3)&~std::size_t(3);
    std::size_t end=first+((accepted+3)&~std::size_t(3));
    for(std::size_t lane=first;lane<end;) {
        if(age(lane)>100.f) {
            copy(lane,end-1);--end;
            if(accepted)--accepted;
        } else ++lane;
    }
    const std::size_t moved=std::min(first-previous,accepted);
    const std::size_t source=first+accepted-moved;
    const std::size_t available=end>source?std::min(moved,end-source):0;
    for(std::size_t lane=0;lane<available;++lane)copy(previous+lane,source+lane);
    return previous+accepted;
}
inline std::uint64_t advanceDistanceEmission(EmissionState &state, float rate,
    const std::array<float,3> &velocity, float delta)
{
    if (rate == 0) return 0;
    state.random.value();
    volatile float xx=velocity[0]*velocity[0], yy=velocity[1]*velocity[1], zz=velocity[2]*velocity[2];
    volatile float lengthSquared=xx+yy; lengthSquared+=zz;
    volatile float weighted=std::max(rate,0.f)*delta;
    volatile float packet=std::sqrt(float(lengthSquared))*weighted;
    volatile float accumulated=packet+state.carry;
    const auto count=std::uint64_t(accumulated);
    volatile float remainder=accumulated-float(count);
    state.carry=remainder;
    state.intervalScale=packet>=9.999999747378752e-5f?1.f/packet:1.f;
    return count;
}
inline EmissionAmount advanceConstantEmission(EmissionState &state,const ConstantEmission &module,
                                               float previous,float current,float duration) {
    state.random.value(); // 即使为 Constant 或零 rate，源模块仍推进状态。
    const float begin=std::max(previous,0.f),end=std::max(current,0.f);
    float packet=0;
    if(module.rate>0) {
        float until=end;
        if(begin>end) {volatile float amount=module.rate*end;packet=amount;until=duration;}
        volatile float delta=until-begin,amount=std::max(module.rate,0.f)*delta;
        packet+=amount;
    }
    int bursts=0;
    if(begin>end) {
        bursts=countBursts(module,0,end,true);
        volatile float through=duration+9.999999747378752e-5f;
        bursts+=countBursts(module,begin,through,false);
    } else bursts=countBursts(module,begin,end,true);
    volatile float accumulated=packet+state.carry;
    state.carry=accumulated;
    const auto rate=std::uint64_t(accumulated);
    volatile float remainder=state.carry-float(rate);
    state.carry=remainder;
    state.intervalScale=packet>=9.999999747378752e-5f?1.f/packet:1.f;
    return {rate,rate+std::uint64_t(bursts)};
}
}
