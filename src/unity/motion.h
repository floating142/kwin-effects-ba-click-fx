// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: ShittimParticleMotion.h.
// Native reference: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleMotion.h
 * @brief 按源百分数年龄、时间步和局部重力推进粒子运动。
 */
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>

namespace baclickfx::unity {
using MotionVector=std::array<float,3>;
using MotionMatrix3=std::array<MotionVector,3>; // 按列存储
// UnityPlayer 粒子年龄以百分数表示；保留 dt*100、乘 inverseLifetime 和累加时的 float32 舍入。
inline float advancePercentAge(float age,float dt,float inverseLifetime,
                               float limit=100.00000762939453f) {
    if(age>100.f)return age;
    volatile float scaled=100.f*dt,increment=inverseLifetime*scaled;
    return std::min(float(increment+age),limit);
}
inline float normalizedAge(float percentAge) {
    return percentAge*.009999999776482582f;
}
struct Motion {
    MotionVector position{},velocity{};
};
struct PrewarmPlan {float wallSeconds=0,phase=0,simulationWindow=0;};
inline PrewarmPlan prewarmPlan(float maximumLifetime,float duration,float speed) {
    const float lifetime=maximumLifetime==std::numeric_limits<float>::infinity()?duration:maximumLifetime;
    const float wall=lifetime/std::max(.0010000000474974513f,speed);
    volatile float begin=-lifetime,ratio=lifetime/duration;
    const float rounded=std::ceil(float(ratio));
    const auto count=(!std::isfinite(rounded) || rounded>=2147483648.f)
            ?std::numeric_limits<std::int32_t>::min():std::int32_t(rounded);
    const float cycles=float(count);
    volatile float offset=cycles*duration;begin+=offset;
    return {wall,std::fmod(float(begin),duration),wall*speed};
}
// ae07f0 的非发射追赶步选择；固定步长模式关闭大积压加速，普通与固定调度均由调用方传入。
inline float simulationStep(float delta,float maximum) {
    if(maximum<=0 || delta<=maximum)return delta;
    volatile float ratio=delta/maximum,count=std::ceil(float(ratio));
    return delta/count;
}
template<class Step> float catchup(float remaining,float initialStep,float duration,bool fixedStep,Step step,bool wholeSteps=false) {
    if(initialStep<=0)return remaining;
    float dt=initialStep;
    const float minimum=wholeSteps?initialStep:1e-6f;
    while(remaining>=minimum) {
        const float next=std::min(remaining,initialStep);
        if(!fixedStep && remaining>10.f) {
            if(dt<=1.f)dt=std::min(duration,1.f);
        } else if(!fixedStep && remaining>5.f) {
            if(dt<=.20000000298023224f)dt=std::min(duration,.20000000298023224f);
        } else dt=next;
        if(dt<=0)return remaining;
        step(dt);
        remaining-=dt;
    }
    return remaining;
}
// InitialModule 在位置步进前把重力加到持久速度；常量路径先乘 modifier*dt，再乘世界重力。
inline void applyGravity(Motion &motion,const MotionVector &gravity,float modifier,float dt) {
    if(modifier==0)return;
    volatile float xx=gravity[0]*gravity[0],yy=gravity[1]*gravity[1],zz=gravity[2]*gravity[2];
    volatile float xy=xx+yy,length=xy+zz;
    if(length<=9.99999905104687e-9f)return;
    volatile float weighted=modifier*dt;
    for(int axis=0;axis<3;++axis) {
        volatile float delta=gravity[std::size_t(axis)]*weighted;
        motion.velocity[std::size_t(axis)]+=delta;
    }
}
// 源 Local 模拟先按 modifier*dt 加权世界重力，再用发射器逆旋转变换该增量；把矩阵乘法移到时间步之前会改变 float32 舍入。
inline void applyLocalGravity(Motion &motion,const MotionVector &gravity,float modifier,float dt,
                              const MotionMatrix3 &inverseRotation) {
    if(modifier==0)return;
    Motion increment{};applyGravity(increment,gravity,modifier,dt);
    for(std::size_t axis=0;axis<3;++axis) {
        volatile float x=increment.velocity[0]*inverseRotation[0][axis];
        volatile float y=increment.velocity[1]*inverseRotation[1][axis];
        volatile float z=increment.velocity[2]*inverseRotation[2][axis];
        volatile float tail=axis==1?z+y:y+z;
        volatile float delta=tail+x;
        motion.velocity[axis]+=delta;
    }
}
inline void integrateMotion(Motion &motion,const MotionVector &moduleVelocity,float dt) {
    for(int axis=0;axis<3;++axis) {
        volatile float velocity=moduleVelocity[std::size_t(axis)]+motion.velocity[std::size_t(axis)];
        volatile float delta=velocity*dt;
        motion.position[std::size_t(axis)]+=delta;
    }
}
}
