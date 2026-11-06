// SPDX-License-Identifier: GPL-3.0-or-later

#include "clickinstance.h"
#include "curveutils.h"
#include "unity/curve.h"
#include "unity/rotation.h"
#include "unity/timing.h"
#include <span>

#include <algorithm>
#include <cmath>

namespace KWin
{

using baclickfx::clamp;
using baclickfx::jsRound;

namespace
{

// 生成兼容与诊断使用的毫秒寿命，不参与动画进度计算。
int lifetimeMsOf(double lifetimeSec, double timeScale)
{
    return std::max(16, static_cast<int>(jsRound((lifetimeSec * 1000) / timeScale)));
}

// 将 Unity 模拟域寿命换算为墙钟时长。
double durationSecOf(double lifetimeSec, double timeScale)
{
    return timeScale > 0 ? lifetimeSec / timeScale : lifetimeSec;
}

// 发射数据构建

baclickfx::unity::Config initialConfig(const baclickfx::Subsystem &params)
{
    baclickfx::unity::Config result;
    result.lifetime = {float(params.startLifetimeMinScalar), float(params.startLifetimeScalar), params.startLifetimeMode};
    result.sizeX = {float(params.startSizeMinScalar), float(params.startSizeScalar), params.startSizeMode};
    result.rotationZ = {float(params.startRotationMinScalar), float(params.startRotationScalar), params.startRotationMode};
    return result;
}

RingEmission emitRing(const baclickfx::Subsystem &params, double timeScale, baclickfx::Rng &rng)
{
    RingEmission e;
    e.params = params;
    baclickfx::unity::Initial initial(rng.nextSeed());
    const auto sampled = initial.next(initialConfig(params));
    e.lifetimeSec = sampled.lifetime;
    e.inverseLifetime = sampled.inverseLifetime;
    e.durationSec = durationSecOf(e.lifetimeSec, timeScale);
    e.lifetimeMs = lifetimeMsOf(e.lifetimeSec, timeScale);
    e.startRotationRad = -sampled.rotation[2];
    e.startSize = sampled.size[0];
    return e;
}

MeshTriEmission emitMeshTri(const baclickfx::Subsystem &params, double timeScale, baclickfx::Rng &rng)
{
    MeshTriEmission e;
    e.params = params;
    e.lifetimeSec = params.startLifetimeScalar;
    e.inverseLifetime = baclickfx::unity::inverseLifetime(float(e.lifetimeSec));
    e.durationSec = durationSecOf(e.lifetimeSec, timeScale);
    e.lifetimeMs = lifetimeMsOf(e.lifetimeSec, timeScale);

    const int count = std::max(0, static_cast<int>(jsRound(params.clickBurst)));
    baclickfx::unity::Initial initial(rng.nextSeed());
    e.arcs.reserve(count);
    for (int i = 0; i < count; i++) {
        MeshTriArc arc;
        const auto sampled = initial.next(initialConfig(params));
        arc.angle = -sampled.rotation[2];
        arc.seed = sampled.seed;
        arc.radiusMul = sampled.size[0] / params.size;
        e.arcs.push_back(arc);
    }
    return e;
}


} // namespace

// 时间与角度求值

void advanceMeshTri(MeshTriEmission &sub, double simulationDeltaSec)
{
    if (simulationDeltaSec <= 0 || !baclickfx::unity::survives(sub.percentAge)) return;
    const float dt = float(simulationDeltaSec);
    const float progress = std::max(0.f, sub.percentAge * .01f);
    if (sub.params.rotationEnabled) {
        for (auto &arc : sub.arcs) {
            const float speed = baclickfx::unity::rotationSpeed(
                std::span(sub.params.rotationCurveMin.data, sub.params.rotationCurveMin.size),
                std::span(sub.params.rotationCurveMax.data, sub.params.rotationCurveMax.size),
                float(sub.params.rotationScalar), progress, arc.seed);
            volatile float delta = speed * dt;
            // Unity 在世界角度上逐次舍入，投影到 y 向下的屏幕时只做一次反号。
            arc.angle = -float(float(-arc.angle) + delta);
        }
    }
    sub.percentAge = baclickfx::unity::advancePercentAge(sub.percentAge, dt, sub.inverseLifetime);
}

double meshTriCustom0(const MeshTriEmission &sub, double p)
{
    return baclickfx::unity::evaluateParticleCurve(
        std::span(sub.params.customData0Curve.data, sub.params.customData0Curve.size), float(p));
}

float particleProgress(float percentAge)
{
    return std::max(0.f, baclickfx::unity::normalizedAge(percentAge));
}

namespace {
void advanceTriBurstStep(TriBurstEmission &burst, float dt)
{
    for (auto &particle : burst.particles) {
        if (!baclickfx::unity::survives(particle.percentAge)) continue;
        baclickfx::unity::integrateMotion(particle.worldMotion, {}, dt);
        particle.percentAge = baclickfx::unity::advancePercentAge(particle.percentAge, dt, particle.inverseLifetime);
    }
    const std::size_t count = baclickfx::unity::compactDead(burst.particles.size(),
        [&](std::size_t i) { return burst.particles[i].percentAge; },
        [&](std::size_t destination, std::size_t source) { burst.particles[destination] = burst.particles[source]; });
    burst.particles.resize(count);
}

template<typename Step>
void advanceFrame(float &remainder, double wallDeltaSec, double timeScale, Step step)
{
    const float delta = float(wallDeltaSec * timeScale);
    if (!(delta > 0)) return;
    const float maximum = baclickfx::unity::sourceMaximumParticleTimestep;
    const float tick = baclickfx::unity::simulationStep(delta, maximum);
    remainder = baclickfx::unity::catchup(remainder + delta, tick, 1.f, false, step);
}
}

void initializeClickBirth(ClickInstance &instance, double wallDeltaSec)
{
    bool first = true;
    advanceFrame(instance.simulationRemainder, wallDeltaSec, instance.timeScale, [&](float dt) {
        if (first) {
            const float partial = baclickfx::unity::birthTimestep(0, 0, baclickfx::unity::EmissionState(), dt);
            instance.ring.percentAge = baclickfx::unity::advancePercentAge(0, partial, instance.ring.inverseLifetime);
            advanceMeshTri(instance.meshTri, partial);
            ParticleBirthFrame frame;
            frame.step = dt;
            for (std::size_t lane = 0; lane < instance.ring3.particles.size(); ++lane)
                applyTriParticleBirth(instance.ring3.particles[lane], lane, frame);
            first = false;
        } else {
            instance.ring.percentAge = baclickfx::unity::advancePercentAge(instance.ring.percentAge,
                dt, instance.ring.inverseLifetime);
            advanceMeshTri(instance.meshTri, dt);
            advanceTriBurstStep(instance.ring3, dt);
        }
    });
    instance.birthPending = false;
}

void advanceClickInstance(ClickInstance &instance, double wallDeltaSec)
{
    advanceFrame(instance.simulationRemainder, wallDeltaSec, instance.timeScale, [&](float dt) {
        auto &ring = instance.ring;
        ring.percentAge = baclickfx::unity::advancePercentAge(ring.percentAge, dt, ring.inverseLifetime);
        advanceMeshTri(instance.meshTri, dt);
        advanceTriBurstStep(instance.ring3, dt);
    });
}

void advanceClickFrame(ClickInstance &instance, double wallDeltaSec)
{
    // 空闲后的首个呈现帧也会绘制出生状态。下一帧应正常推进已有粒子。
    if (instance.birthPending) {
        initializeClickBirth(instance, wallDeltaSec);
    } else {
        advanceClickInstance(instance, wallDeltaSec);
    }
    instance.age += wallDeltaSec;
}

void advanceTriBurstFrame(TriBurstEmission &burst, double wallDeltaSec)
{
    advanceFrame(burst.simulationRemainder, wallDeltaSec, burst.timeScale,
        [&](float dt) { advanceTriBurstStep(burst, dt); });
}

bool clickInstanceAlive(const ClickInstance &instance)
{
    return baclickfx::unity::survives(instance.ring.percentAge)
        || baclickfx::unity::survives(instance.meshTri.percentAge)
        || !instance.ring3.particles.empty();
}

// 三角粒子簇发射

QPointF triParticleOffset(const TriParticle &particle)
{
    const auto &p = particle.worldMotion.position;
    return {p[0] * particle.worldUnitPx, -p[1] * particle.worldUnitPx};
}

void applyTriParticleBirth(TriParticle &particle, std::uint64_t lane, const ParticleBirthFrame &frame)
{
    const float fraction = baclickfx::unity::birthFraction(lane, frame.rateCount, frame.emission);
    volatile float interpolationStep = frame.step / frame.simulationSpeed;
    volatile float interpolation = fraction * interpolationStep;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        volatile float shift = frame.emitterVelocity[axis] * interpolation;
        particle.worldMotion.position[axis] -= shift;
    }
    const float partial = baclickfx::unity::birthTimestep(lane, frame.rateCount,
        frame.emission, frame.step, frame.offset);
    baclickfx::unity::integrateMotion(particle.worldMotion, {}, partial);
    particle.percentAge = baclickfx::unity::advancePercentAge(0, partial, particle.inverseLifetime);
}

TriBurstEmission emitTriBurst(const baclickfx::Subsystem &params, double timeScale,
                              int countOverride, baclickfx::Rng &rng, ParticleEmitterState *state)
{
    using namespace baclickfx::unity;
    TriBurstEmission e;
    e.params = params;
    e.timeScale = timeScale;
    const int count = std::max(0, countOverride >= 0 ? countOverride : params.clickBurst);
    if (!count) return e;
    ParticleEmitterState temporary(state ? 0 : rng.nextSeed());
    auto &emitter = state ? *state : temporary;
    // Unity 将同一次发射补齐为四 lane 批次；各模块的随机流互不消耗。
    if (!state) emitter.beginEmission(std::uint64_t(count));
    const Config initial = initialConfig(params);
    const Range speed{float(params.startSpeedMinScalar), float(params.startSpeedScalar), params.startSpeedMode};
    CircleShapeConfig shape;
    shape.scale = {float(params.shapeScale), float(params.shapeScale), 1};
    shape.thickness = 0;
    shape.randomDirection = 1;
    shape.sphericalDirection = 1;
    TextureSheet uv;
    uv.enabled = params.uvEnabled;
    uv.start = {float(params.uvStartFrameMinScalar), float(params.uvStartFrameScalar), params.uvStartFrameMode};
    uv.frame = {float(params.uvFrameOverTimeMinScalar), float(params.uvFrameOverTimeScalar), params.uvFrameOverTimeMode};
    e.particles.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto sampled = emitter.initial.next(initial);
        const auto point = emitter.shape.next(shape);
        const float velocity = startSpeed(sampled.seed, speed);
        TriParticle item;
        item.worldUnitPx = params.worldUnitPx;
        item.billboard = params.billboard;
        item.seed = sampled.seed;
        // World 出生管线在 Shape 内应用模拟矩阵，然后乘 StartSpeed。
        // scalingMode=Local 保留作者的叶节点缩放；不可把它放进会归一化的 Shape 方向。
        const float scale = float(params.localScale);
        item.inverseLifetime = sampled.inverseLifetime;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            item.worldMotion.position[axis] = point.position[axis] * scale;
            volatile float worldDirection = point.direction[axis] * scale;
            item.worldMotion.velocity[axis] = worldDirection * velocity;
        }
        item.lifetimeSec = sampled.lifetime;
        item.durationSec = durationSecOf(item.lifetimeSec, timeScale);
        item.lifetimeMs = lifetimeMsOf(item.lifetimeSec, timeScale);
        item.startSize = sampled.size[0];
        item.orientBase = -sampled.rotation[2];
        // 帧动画的两个随机量按 particleSeed 派生，float32 相加后只取模一次。
        item.uvStartFrameNorm = uv.normalizedFrame(sampled.seed);
        item.uvFrameOverTimeNorm = 0;
        e.durationSec = std::max(e.durationSec, item.durationSec);
        e.lifetimeMs = std::max(e.lifetimeMs, item.lifetimeMs);
        e.particles.push_back(item);
    }
    e.lifetimeSec = e.durationSec * timeScale;
    return e;
}


// 点击实例组装
void appendTriBurst(TriBurstEmission &burst, const baclickfx::Subsystem &params,
    double timeScale, std::uint64_t requested, baclickfx::Rng &rng, ParticleEmitterState &state,
    const ParticleBirthFrame &frame, const baclickfx::unity::MotionVector &originOffset)
{
    if (params.maxParticles > 0) {
        const auto live = std::ranges::count_if(burst.particles, [](const TriParticle &particle) {
            return baclickfx::unity::survives(particle.percentAge);
        });
        const auto available = std::max<std::ptrdiff_t>(0, params.maxParticles - live);
        requested = std::min(requested, std::uint64_t(available));
    }
    const int accepted = int(requested);
    if (accepted <= 0) return;
    const std::size_t previous = burst.particles.size();
    const std::size_t first = (previous + 3) & ~std::size_t(3);
    const int padded = (accepted + 3) & ~3;
    state.beginEmission(std::uint64_t(accepted));
    auto newborn = emitTriBurst(params, timeScale, padded, rng, &state);
    burst.particles.resize(first + std::size_t(padded));
    for (int lane = 0; lane < padded; ++lane) {
        auto particle = newborn.particles[std::size_t(lane)];
        for (std::size_t axis = 0; axis < 3; ++axis)
            particle.worldMotion.position[axis] += originOffset[axis];
        applyTriParticleBirth(particle, std::uint64_t(lane), frame);
        burst.particles[first + std::size_t(lane)] = particle;
    }
    const auto count = baclickfx::unity::finishBirthStorage(previous, std::size_t(accepted),
        [&](std::size_t i) { return burst.particles[i].percentAge; },
        [&](std::size_t to, std::size_t from) { burst.particles[to] = burst.particles[from]; });
    burst.particles.resize(count);
    burst.params = params;
    burst.timeScale = timeScale;
    burst.durationSec = std::max(burst.durationSec, newborn.durationSec);
    burst.lifetimeSec = std::max(burst.lifetimeSec, newborn.lifetimeSec);
    burst.lifetimeMs = std::max(burst.lifetimeMs, newborn.lifetimeMs);
}


ClickInstance makeClickInstance(const QPointF &pos, const baclickfx::SubsystemMap &ss,
                                double timeScale, baclickfx::Rng &rng)
{
    ClickInstance inst;
    inst.center = pos;
    inst.timeScale = timeScale;
    // 保守包围盒覆盖以点击点为中心的两个 Unity 世界单位。
    inst.halfSize = jsRound(2.0 * ss.ring.worldUnitPx) / 2;

    inst.ring = emitRing(ss.ring, timeScale, rng);
    inst.ring3 = emitTriBurst(ss.ring3, timeScale, -1, rng);
    inst.meshTri = emitMeshTri(ss.meshTri, timeScale, rng);

    // 回收边界使用各层已经算出的精确墙钟时长。lifetimeMs 是早期 JS 定时器
    // 兼容值，经过毫秒四舍五入，用它回收会让连续随机寿命最多提前 0.5ms 消失。
    inst.life = std::max({inst.ring.durationSec,
                          inst.ring3.durationSec,
                          inst.meshTri.durationSec});
    return inst;
}

} // namespace KWin
