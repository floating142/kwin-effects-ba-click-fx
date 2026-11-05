// SPDX-License-Identifier: GPL-3.0-or-later
// Unity 粒子系统参数及逻辑像素域派生量。

#pragma once

#include "curvedata.h"
#include "curveutils.h"
#include "unity/gradient.h"
#include <memory>

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
    double sizeCurveMultiplier = 1.0;
    int renderQueue = 3000;
    float minParticleSize = 0, maxParticleSize = 1;
    float billboardContextScale = 1, cameraWorldUnitPx = 540, cameraHorizontalSpan = 3.5555556f;
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

    // 起始色先量化为 RGBA8，与缓存寿命色相乘后再解码至线性空间。
    Rgb startColor{1.0, 1.0, 1.0};
    std::shared_ptr<const unity::Gradient> particleGradient;

    // ParticleSystem.maxNumParticles 作用于单个 ParticleSystem 实例。达到上限时
    // Unity 跳过新粒子；0 表示不限制。
    int maxParticles = 0;

    // 派生参数，由 buildSubsystemMap() 统一填充。
    // 当前输出上一个 Unity 世界单位对应的逻辑像素数：
    //   worldUnitPx = outputHeightPx / (2 * kUnityOrthographicSize) * globalScale
    // 所有像素域参数、发射间距和包围盒计算均从该值派生。
    double worldUnitPx = 0.0;
    int lifetimeMs = 0;
    double sizePx = 0.0;
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
                               double outputHeightPx = kReferenceHeightPx, double outputWidthPx = 1920.0);

/// 采用原生 billboard min/max 裁限，返回投影后的逻辑像素半尺寸。
double billboardHalfSize(const Subsystem &params, float startSize, float curve, float percentAge);

// 采样辅助函数

/// 计算 `[0, 1]` 范围内的生命周期进度。
double subProgress(double lifetimeMs, double tMs);

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
 * 先按原生 RGBA8 规则组合 startColor 与寿命渐变，再将最终 RGB 解码至线性。
 */
Rgba subColor(const Subsystem &p, double progress, double fallbackAlpha = 1.0);

} // namespace baclickfx
