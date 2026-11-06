// SPDX-License-Identifier: GPL-3.0-or-later
// 点击实例的发射数据以及各层时间、角度求值。

#pragma once

#include "render/meshprofiles.h"
#include "subsystems.h"
#include "unity/initial.h"
#include "unity/emission.h"
#include "unity/circle.h"
#include "unity/uv.h"
#include "unity/motion.h"
#include "unity/retirement.h"

#include <QPointF>

#include <vector>

namespace KWin
{

// 发射数据
//
// `lifetimeSec` 表示 Unity 模拟域寿命，用于计算位移；`durationSec` 表示经过
// `timeScale` 换算后的参考墙钟时长。实际进度和退役采用原生 percentAge。
// `lifetimeMs` 仅用于兼容和诊断。

/// Ring 圆盘的发射参数。
struct RingEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    double startRotationRad = 0.0;
    float startSize = 0;
    float percentAge = 0;
    float inverseLifetime = 0;
};

/// Ring3 与 Ring4 共用的单个三角粒子参数。
struct TriParticle
{
    baclickfx::unity::Motion worldMotion;
    // 同一 Ring4 发射器可跨输出，已出生粒子保留原有投影。
    double worldUnitPx = 0;
    baclickfx::Subsystem::BillboardProjection billboard;
    float percentAge = 0;
    float inverseLifetime = 0;
    std::uint32_t seed = 0;
    // PS17/PS18 的初始旋转恒为 0；三角朝向由图集帧决定。
    double orientBase = 0.0;
    double spin = 0.0; // PS17/PS18 的 RotationModule 已禁用。
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    float startSize = 0;
    double uvStartFrameNorm = 0.0;
    double uvFrameOverTimeNorm = 0.0;
};

/// 每个 ParticleSystem 独立的 Initial / Shape 四 lane 随机流。
struct ParticleEmitterState
{
    explicit ParticleEmitterState(std::uint32_t seed = 0) : initial(seed), shape(seed) { emission.init(seed); }
    baclickfx::unity::Initial initial;
    baclickfx::unity::CircleShape shape;
    baclickfx::unity::EmissionState emission;
    void beginEmission(std::uint64_t count) { initial.discardBatch(); shape.beginEmission(count); }
};

struct ParticleBirthFrame
{
    baclickfx::unity::EmissionState emission;
    std::uint64_t rateCount = 0;
    float step = 0, offset = 0, simulationSpeed = 1;
    baclickfx::unity::MotionVector emitterVelocity{};
};
void applyTriParticleBirth(TriParticle &particle, std::uint64_t lane, const ParticleBirthFrame &frame);

/// World 模拟空间：出生位置与速度已由原生出生管线变换，投影时不重复乘局部缩放。
QPointF triParticleOffset(const TriParticle &particle);

struct TriBurstEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    std::vector<TriParticle> particles;
    double timeScale = 1;
    float simulationRemainder = 0;
};

void appendTriBurst(TriBurstEmission &burst, const baclickfx::Subsystem &params,
    double timeScale, std::uint64_t requested, baclickfx::Rng &rng, ParticleEmitterState &state,
    const ParticleBirthFrame &frame, const baclickfx::unity::MotionVector &originOffset = {});

/// MeshTri 的单个圆环弧段参数。
struct MeshTriArc
{
    double angle = 0.0;
    std::uint32_t seed = 0;
    double radiusMul = 1.0;
};

struct MeshTriEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    float percentAge = 0;
    float inverseLifetime = 0;
    std::vector<MeshTriArc> arcs;
};

// 点击实例

/// 一次完整点击产生的全部可见发射数据。
struct ClickInstance
{
    QPointF center;        // 全局逻辑坐标。
    double age = 0.0;      // 由帧时钟推进的墙钟时间，单位为秒。
    double life = 0.0;     // 精确墙钟寿命，单位为秒。
    double halfSize = 0.0; // 保守包围盒半边长。
    // 首次呈现时按源出生分数推进，而不是额外停留一帧。
    bool birthPending = true;
    double timeScale = 1;
    float simulationRemainder = 0;

    RingEmission ring;
    TriBurstEmission ring3;
    MeshTriEmission meshTri;
};

/**
 * 创建点击实例并完成所有随机参数采样。
 *
 * @param pos 点击位置，使用全局逻辑坐标。
 * @param ss 已包含输出尺寸换算的子系统参数。
 * @param timeScale 动画时间缩放。
 * @param rng 随机数源。
 * @return 可直接加入渲染队列的点击实例。
 */
ClickInstance makeClickInstance(const QPointF &pos, const baclickfx::SubsystemMap &ss,
                                double timeScale, baclickfx::Rng &rng);

/**
 * 创建 Ring3 或 Ring4 的三角粒子簇。
 *
 * @param params 粒子子系统参数。
 * @param timeScale 动画时间缩放。
 * @param countOverride 粒子数量；负数表示使用 `params.clickBurst`。
 * @param rng 随机数源。
 */
TriBurstEmission emitTriBurst(const baclickfx::Subsystem &params, double timeScale,
                              int countOverride, baclickfx::Rng &rng,
                              ParticleEmitterState *state = nullptr);

// 求值辅助函数
//
// 模拟、damage 计算与 GPU 渲染共享这些纯函数，确保使用同一套公式。

/// 用原生百分比年龄采样曲线；存活与回收仍以原始百分比判定。
float particleProgress(float percentAge);
bool clickInstanceAlive(const ClickInstance &instance);
void initializeClickBirth(ClickInstance &instance, double wallDeltaSec);
void advanceClickFrame(ClickInstance &instance, double wallDeltaSec);
void advanceClickInstance(ClickInstance &instance, double wallDeltaSec);
void advanceTriBurstFrame(TriBurstEmission &burst, double wallDeltaSec);

/// 按 Unity World 模式逐帧求角速度并推进角度及百分比年龄。
void advanceMeshTri(MeshTriEmission &sub, double simulationDeltaSec);

/// 采样驱动 MeshTri 溶解范围的 CustomData0 曲线。
double meshTriCustom0(const MeshTriEmission &sub, double p);

} // namespace KWin
