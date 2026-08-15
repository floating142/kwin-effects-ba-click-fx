// SPDX-License-Identifier: GPL-3.0-or-later
// Unity 粒子系统参数及逻辑像素域派生量。

#pragma once

#include "curvedata.h"
#include "curveutils.h"

namespace baclickfx {

/// 单个 Unity 粒子或拖尾子系统的运行时参数。
struct Subsystem {
    /// 表示可选字段未设置，由构建阶段填入对应默认值。
    static constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();

    // 基础参数
    double lifetimeSec = 0.0001;
    double size = 0.0;
    double speed = 0.0;
    double localScale = 1.0;
    double rateOverDistance = 0.0;
    int clickBurst = 0;
    int distanceBurst = 1;

    // 曲线；空视图使用求值函数的回退值。
    ScalarCurve sizeCurve;
    ColorCurve colorStops;
    ScalarCurve alphaStops;
    int gradientMode = 0;
    ColorCurve colorKeys;
    ScalarCurve alphaKeys;

    // ShapeModule
    bool shapeEnabled = false;
    double shapeScale = 0.0;

    // UVModule
    bool uvEnabled = false;
    int uvTilesX = 1;
    int uvTilesY = 1;
    double uvCycles = 1.0;
    double uvStartFrame = 0.0;
    int uvStartFrameMode = 0;
    double uvStartFrameScalar = kUnset;    // 未设时回落到 uvStartFrame
    double uvStartFrameMinScalar = kUnset; // 同上
    double uvFrameOverTime = 0.0;
    int uvFrameOverTimeMode = 0;
    double uvFrameOverTimeScalar = kUnset;    // 未设时回落到 uvFrameOverTime
    double uvFrameOverTimeMinScalar = kUnset; // 同上

    // InitialModule MinMaxConstant；模式 3 表示在区间内随机采样。
    int startLifetimeMode = 0;
    double startLifetimeScalar = kUnset;    // 未设时回落到 lifetimeSec
    double startLifetimeMinScalar = kUnset; // 同上
    int startSpeedMode = 0;
    double startSpeedScalar = kUnset;    // 未设时回落到 speed
    double startSpeedMinScalar = kUnset; // 同上
    int startSizeMode = 0;
    double startSizeScalar = kUnset;    // 未设时回落到 size
    double startSizeMinScalar = kUnset; // 同上
    int startRotationMode = 0;
    double startRotationScalar = 0.0;
    double startRotationMinScalar = 0.0;

    // RotationModule 与 CustomDataModule
    bool rotationEnabled = false;
    double rotationScalar = 0.0;
    ScalarCurve rotationCurveMin;
    ScalarCurve rotationCurveMax;
    ScalarCurve customData0Curve;

    // TrailRenderer
    double widthMultiplier = 0.0;
    ScalarCurve widthCurve; // 未设时回落到恒 1 曲线
    double minVertexDistance = 0.0;
    int numCornerVertices = 0;
    int numCapVertices = 0;
    int textureMode = 0;
    double textureScale = 1.0;
    double killUnderTimeScale = 0.0;
    Rgb hdrColor{1.0, 1.0, 1.0};
    const char *textureAsset = nullptr;
    bool textureClamp = true;

    // InitialModule.startColor 与 ColorOverLifetime 在线性空间相乘。Ring3/Ring4 的
    // 0.5377358 灰色在 Apply Active Color Space 后约为 0.2506 线性能量。
    Rgb startColor{1.0, 1.0, 1.0};

    // ParticleSystem.maxNumParticles 作用于单个 ParticleSystem 实例。达到上限时
    // Unity 跳过新粒子；0 表示不限制。
    int maxParticles = 0;

    // 派生参数，由 buildSubsystemMap() 统一填充。
    // 当前输出上一个 Unity 世界单位对应的逻辑像素数：
    //   worldUnitPx = kUnitySizeToPx * (outputHeightPx / kReferenceHeightPx) * globalScale
    //               = outputHeightPx / 2 * globalScale
    // 所有像素域参数、发射间距和包围盒计算均从该值派生。
    double worldUnitPx = 0.0;
    int lifetimeMs = 0;
    double localScalePx = 0.0;
    double sizePx = 0.0;
    double speedPxPerSec = 0.0;
    double widthPx = 0.0;
    double minVertexDistancePx = 0.0;
    double hdrGain = 1.0;
};

/// FX_Touch 中五个可见子系统的运行时参数集合。
struct SubsystemMap {
    Subsystem ring;
    Subsystem ring3;
    Subsystem ring4;
    Subsystem meshTri;
    Subsystem trail;
};

/**
 * 构建五个可见子系统的运行时参数。
 *
 * @param timeScale 动画时间缩放。
 * @param globalScale 全局尺寸缩放。
 * @param outputHeightPx 输出逻辑高度；非正值使用参考高度。
 * @return 已完成逻辑像素换算的参数集合。
 */
SubsystemMap buildSubsystemMap(double timeScale, double globalScale,
                               double outputHeightPx = kReferenceHeightPx);

// 采样辅助函数

/// 计算 `[0, 1]` 范围内的生命周期进度。
double subProgress(double lifetimeMs, double tMs);

/// 采样 Unity MinMaxConstant；模式 3 使用两个端点之间的均匀随机值。
double sampleMinMaxConstant(int mode, double scalar, double minScalar, Rng &rng);

/// 按 startLifetime 模式采样生命周期，单位为秒。
double sampleStartLifetimeSec(const Subsystem &p, Rng &rng);

/// 采样相对 `Subsystem::size` 的初始尺寸倍率。
double sampleStartSizeMul(const Subsystem &p, Rng &rng);

/// 采样粒子初始旋转角，单位为弧度。
double sampleStartRotation(const Subsystem &p, Rng &rng);

/// 采样粒子初始速度，单位保持为 Unity 世界单位每秒。
double sampleStartSpeedPx(const Subsystem &p, Rng &rng);

/// 采样并限制每粒子的归一化 UV 起始帧。
double sampleUvStartFrameNorm(const Subsystem &p, Rng &rng);

/// 采样每粒子的非负 UV frame-over-time 值。
double sampleUvFrameOverTimeNorm(const Subsystem &p, Rng &rng);

/// 纹理图集中单帧的 UV 缩放和偏移。
struct UvTile {
    double scaleX = 1.0;
    double scaleY = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;

    /// 将基础 UV 映射到当前图集帧。
    double mapU(double u) const { return offsetX + u * scaleX; }
    double mapV(double v) const { return offsetY + v * scaleY; }
};

/**
 * 计算 Unity UVModule 当前使用的图集帧。
 *
 * 先计算 `startFrame + frameOverTime * cycles`，再重复到 `[0, 1)` 并离散到
 * `tilesX * tilesY`。UVModule 不在相邻帧之间插值。
 */
UvTile uvFrameTile(const Subsystem &p, double startFrameNorm, double frameOverTimeValue);

/// 采样当前生命周期进度对应的尺寸倍率。
double subSizeCurve(const Subsystem &p, double progress);

/// 线性 RGB 与线性 Alpha 颜色。
struct Rgba {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;
};

/**
 * 采样颜色与透明度曲线。
 *
 * 返回的 RGB 已从 sRGB 解码并乘以 `startColor`；Alpha 保持线性值。
 */
Rgba subColor(const Subsystem &p, double progress, double fallbackAlpha = 1.0);

} // namespace baclickfx
