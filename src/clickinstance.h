// SPDX-License-Identifier: GPL-3.0-or-later
// 点击实例的发射数据以及各层时间、角度求值。

#pragma once

#include "meshprofiles.h"
#include "subsystems.h"

#include <QPointF>

#include <vector>

namespace KWin
{

// 发射数据
//
// `lifetimeSec` 表示 Unity 模拟域寿命，用于计算位移；`durationSec` 表示经过
// `timeScale` 换算后的墙钟时长，用于计算动画进度。`lifetimeMs` 仅用于兼容和诊断。

/// Ring 圆盘的发射参数。
struct RingEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    double startRotationRad = 0.0;
};

/// Ring3 与 Ring4 共用的单个三角粒子参数。
struct TriParticle
{
    double angle = 0.0; // 出生位置角度和径向速度方向。
    // PS17/PS18 的初始旋转恒为 0；三角朝向由图集帧决定。
    double orientBase = 0.0;
    double spin = 0.0; // PS17/PS18 的 RotationModule 已禁用。
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    double speedPxPerSec = 0.0;
    double sizeMul = 1.0;
    double uvStartFrameNorm = 0.0;
    double uvFrameOverTimeNorm = 0.0;
};

struct TriBurstEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
    std::vector<TriParticle> particles;
};

/// MeshTri 的单个圆环弧段参数。
struct MeshTriArc
{
    double angle = 0.0;
    double rotMix = 0.0; // rotationCurveMin 与 rotationCurveMax 的混合因子。
    double radiusMul = 1.0;
    double widthMul = 1.0;
    double phase = 0.0;
};

struct MeshTriEmission
{
    baclickfx::Subsystem params;
    double lifetimeSec = 0.0;
    double durationSec = 0.0;
    int lifetimeMs = 16;
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
    // 首帧以 age=0 绘制，之后再按呈现间隔推进。
    bool drawn = false;

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
                              int countOverride, baclickfx::Rng &rng);

// 求值辅助函数
//
// 模拟、damage 计算与 GPU 渲染共享这些纯函数，确保使用同一套公式。

/// 计算图层归一化进度；`durationSec <= 0` 时返回 1。
double layerProgress(double durationSec, double ageSec);

/**
 * 计算 MeshTri 弧段的累计旋转角。
 *
 * Rotation over Lifetime 表示角速度曲线；实现对分段 Hermite 曲线做解析积分，并
 * 在结果上应用屏幕坐标系方向修正。
 */
double meshTriRotation(const MeshTriEmission &sub, double p, double mix);

/// 采样驱动 MeshTri 溶解范围的 CustomData0 曲线。
double meshTriCustom0(const MeshTriEmission &sub, double p);

} // namespace KWin
