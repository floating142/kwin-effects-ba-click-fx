// SPDX-License-Identifier: GPL-3.0-or-later

#include "clickinstance.h"
#include "curveutils.h"

#include <algorithm>
#include <cmath>

namespace KWin
{

using baclickfx::Rgb;
using baclickfx::Rgba;
using baclickfx::clamp;
using baclickfx::lerp;

namespace
{

// JavaScript Math.round 采用 floor(x + 0.5)，其负半数行为与 std::round 不同。
double jsRound(double v)
{
    return std::floor(v + 0.5);
}

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

RingEmission emitRing(const baclickfx::Subsystem &params, double timeScale, baclickfx::Rng &rng)
{
    RingEmission e;
    e.params = params;
    e.lifetimeSec = baclickfx::sampleStartLifetimeSec(params, rng);
    e.durationSec = durationSecOf(e.lifetimeSec, timeScale);
    e.lifetimeMs = lifetimeMsOf(e.lifetimeSec, timeScale);
    e.startRotationRad = baclickfx::sampleStartRotation(params, rng);
    return e;
}

MeshTriEmission emitMeshTri(const baclickfx::Subsystem &params, double timeScale, baclickfx::Rng &rng)
{
    MeshTriEmission e;
    e.params = params;
    e.lifetimeSec = baclickfx::sampleStartLifetimeSec(params, rng);
    e.durationSec = durationSecOf(e.lifetimeSec, timeScale);
    e.lifetimeMs = lifetimeMsOf(e.lifetimeSec, timeScale);

    const int count = std::max(0, static_cast<int>(jsRound(params.clickBurst)));
    e.arcs.reserve(count);
    for (int i = 0; i < count; i++) {
        MeshTriArc arc;
        arc.angle = baclickfx::sampleStartRotation(params, rng);
        arc.rotMix = rng.range(0.0, 1.0);
        arc.radiusMul = baclickfx::sampleStartSizeMul(params, rng);
        arc.widthMul = 1.0;
        arc.phase = 0.0;
        e.arcs.push_back(arc);
    }
    return e;
}


} // namespace

// 时间与角度求值

double layerProgress(double durationSec, double ageSec)
{
    if (durationSec <= 0) {
        return 1.0;
    }
    return clamp(ageSec / durationSec, 0, 1);
}

double meshTriRotation(const MeshTriEmission &sub, double p, double mix)
{
    if (!sub.params.rotationEnabled || p <= 0) {
        return 0;
    }

    const double lifeSec = sub.lifetimeSec;
    if (lifeSec <= 0) {
        return 0;
    }

    const double blend = clamp(mix, 0, 1);
    const double scalar = sub.params.rotationScalar;
    // Unity 的 Rotation over Lifetime 是角速度曲线。曲线关键帧是非加权
    // 三次 Hermite，因此直接解析积分，避免固定 8 段梯形近似带来的角度误差。
    const auto integrateCurve = [p](const baclickfx::ScalarCurve &curve) {
        if (curve.empty() || p <= 0.0) {
            return 0.0;
        }
        double result = curve.front().v * std::min(p, curve.front().t);
        double from = std::min(p, curve.front().t);
        for (std::size_t i = 1; i < curve.size && from < p; i++) {
            const baclickfx::ScalarStop &a = curve[i - 1];
            const baclickfx::ScalarStop &b = curve[i];
            const double segStart = std::max(from, a.t);
            const double segEnd = std::min(p, b.t);
            if (segEnd <= segStart) {
                continue;
            }
            const double dt = std::max(1e-6, b.t - a.t);
            const double z0 = baclickfx::clamp((segStart - a.t) / dt, 0.0, 1.0);
            const double z1 = baclickfx::clamp((segEnd - a.t) / dt, 0.0, 1.0);
            const auto primitive = [](double z, double va, double vb) {
                return va * (0.5 * z * z * z * z - z * z * z + z)
                    + vb * (-0.5 * z * z * z * z + z * z * z);
            };
            result += dt * (primitive(z1, a.v, b.v) - primitive(z0, a.v, b.v));
            from = segEnd;
        }
        if (p > from) {
            result += curve.back().v * (p - from);
        }
        return result;
    };

    const double minIntegral = integrateCurve(sub.params.rotationCurveMin);
    const double maxIntegral = integrateCurve(sub.params.rotationCurveMax);
    const double integral = lerp(minIntegral, maxIntegral, blend) * scalar;

    // 屏幕坐标系 y 轴向下，旋转方向做一次反号校正。
    return -integral * lifeSec;
}

double meshTriCustom0(const MeshTriEmission &sub, double p)
{
    return baclickfx::evalScalarStops(sub.params.customData0Curve, p, 1.0);
}

// 三角粒子簇发射

TriBurstEmission emitTriBurst(const baclickfx::Subsystem &params, double timeScale,
                              int countOverride, baclickfx::Rng &rng)
{
    TriBurstEmission e;
    e.params = params;
    double maxDurationSec = 0.0;

    const double rawCount = countOverride >= 0 ? countOverride : params.clickBurst;
    const int count = std::max(0, static_cast<int>(jsRound(rawCount)));
    e.particles.reserve(count);
    for (int i = 0; i < count; i++) {
        TriParticle item;
        item.lifetimeSec = baclickfx::sampleStartLifetimeSec(params, rng);
        item.durationSec = durationSecOf(item.lifetimeSec, timeScale);
        item.lifetimeMs = lifetimeMsOf(item.lifetimeSec, timeScale);
        // ShapeModule 开着，径向随机角。
        item.angle = rng.range(0.0, M_PI * 2);
        // Unity 侧这两层粒子**不转**：PS17/PS18 的 InitialModule.startRotation 是
        // minMaxState=0 / scalar=0，RotationModule 也 enabled=False。
        //
        // Billboard 朝向由 2×1 图集的起始帧决定，几何自身不旋转。
        item.orientBase = 0.0;
        // PS17/PS18 的 RotationModule 关着，没有自转。
        item.spin = 0.0;
        item.speedPxPerSec = baclickfx::sampleStartSpeedPx(params, rng);
        item.sizeMul = baclickfx::sampleStartSizeMul(params, rng);
        item.uvStartFrameNorm = baclickfx::sampleUvStartFrameNorm(params, rng);
        item.uvFrameOverTimeNorm = baclickfx::sampleUvFrameOverTimeNorm(params, rng);
        maxDurationSec = std::max(maxDurationSec, item.durationSec);
        e.particles.push_back(item);
    }

    // 整层的寿命取所有粒子里最长的那个，这样最后一个粒子也能走完。
    int lifetimeMs = 0;
    for (const TriParticle &item : e.particles) {
        lifetimeMs = std::max(lifetimeMs, item.lifetimeMs);
    }
    e.lifetimeMs = lifetimeMs;
    e.durationSec = maxDurationSec;
    e.lifetimeSec = maxDurationSec * timeScale;
    return e;
}


// 点击实例组装

ClickInstance makeClickInstance(const QPointF &pos, const baclickfx::SubsystemMap &ss,
                                double timeScale, baclickfx::Rng &rng)
{
    ClickInstance inst;
    inst.center = pos;
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
