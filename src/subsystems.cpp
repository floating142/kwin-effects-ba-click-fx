// SPDX-License-Identifier: GPL-3.0-or-later

#include "subsystems.h"
#include "touchparams.h"

#include <algorithm>
#include <cmath>
#include <span>
#include "unity/curve.h"
#include "unity/billboard.h"

namespace baclickfx {

using namespace baclickfx::curves;

namespace {

// NaN 表示字段未设置，此时使用对应回退值。
double resolve(double value, double fallback)
{
    return std::isnan(value) ? fallback : value;
}

// widthCurve 的默认值为恒定 1。
constexpr ScalarStop kDefaultWidthCurve[] = {
    {0.0, 1.0},
};

// 补全可选字段并计算逻辑像素域派生量。
void finalize(Subsystem &s, double ts, double worldUnitPx)
{
    // MinMaxConstant 的端点未给出时退化为对应的标量本身。
    s.startLifetimeScalar = resolve(s.startLifetimeScalar, s.lifetimeSec);
    s.startLifetimeMinScalar = resolve(s.startLifetimeMinScalar, s.lifetimeSec);
    s.startSpeedScalar = resolve(s.startSpeedScalar, s.speed);
    s.startSpeedMinScalar = resolve(s.startSpeedMinScalar, s.speed);
    s.startSizeScalar = resolve(s.startSizeScalar, s.size);
    s.startSizeMinScalar = resolve(s.startSizeMinScalar, s.size);

    s.uvStartFrameScalar = resolve(s.uvStartFrameScalar, s.uvStartFrame);
    s.uvStartFrameMinScalar = resolve(s.uvStartFrameMinScalar, s.uvStartFrame);
    s.uvFrameOverTimeScalar = resolve(s.uvFrameOverTimeScalar, s.uvFrameOverTime);
    s.uvFrameOverTimeMinScalar = resolve(s.uvFrameOverTimeMinScalar, s.uvFrameOverTime);

    if (s.widthCurve.empty())
        s.widthCurve = ScalarCurve(kDefaultWidthCurve);

    s.lifetimeMs = static_cast<int>(std::max(16.0, jsRound((s.lifetimeSec * 1000) / ts)));

    // Renderer 尺寸包含局部缩放；World 速度由发射器另行计算。
    s.worldUnitPx = worldUnitPx;
    s.sizePx = s.size * s.localScale * worldUnitPx;
    s.widthPx = s.widthMultiplier * worldUnitPx;
    s.minVertexDistancePx = s.minVertexDistance * worldUnitPx;
    s.hdrGain = (s.hdrColor[0] + s.hdrColor[1] + s.hdrColor[2]) / 3;
    // ColorOverLifetime 缓存使用 uint16 时间和 RGBA8，不在浮点域直接乘色。
    auto gradient = std::make_shared<unity::Gradient>();
    const auto colors = s.colorKeys.empty() ? s.colorStops : s.colorKeys;
    const auto alphas = s.alphaKeys.empty() ? s.alphaStops : s.alphaKeys;
    gradient->enabled = !colors.empty();
    for (std::size_t i = 0; i < colors.size; ++i) {
        const auto &key = colors[i];
        gradient->colors.push_back({std::uint16_t(std::lround(key.t * 65535)),
            {float(key.c[0]), float(key.c[1]), float(key.c[2])}});
    }
    for (std::size_t i = 0; i < alphas.size; ++i) {
        const auto &key = alphas[i];
        gradient->alphas.push_back({std::uint16_t(std::lround(key.t * 65535)), float(key.v)});
    }
    gradient->prepareCache();
    s.particleGradient = std::move(gradient);
}

} // namespace

SubsystemMap buildSubsystemMap(double timeScale, double globalScale, double outputHeightPx, double outputWidthPx)
{
    SubsystemMap m;

    configureTouchSubsystems(m);

    // Unity 固定 UI Pass 的正交相机 orthographicSize = 1.0，垂直视锥范围为
    // 2 * kUnityOrthographicSize 个世界单位。因此世界到屏幕的投影比例为：
    // outputHeight / (2 * orthographicSize)，再应用用户的整体尺寸缩放。
    const double height = outputHeightPx > 0.0 ? outputHeightPx : kReferenceHeightPx;
    const double worldUnitPx = height / (2.0 * kUnityOrthographicSize) * globalScale;

    for (Subsystem *s : {&m.ring, &m.ring3, &m.ring4, &m.meshTri, &m.trail}) {
        finalize(*s, timeScale, worldUnitPx);
        s->billboardContextScale = float(s->localScale) * float(globalScale);
        s->cameraWorldUnitPx = float(height * .5);
        s->cameraHorizontalSpan = float((outputWidthPx > 0 ? outputWidthPx : 1920) / (height * .5));
    }

    return m;
}

double billboardHalfSize(const Subsystem &params, float startSize, float curve, float percentAge)
{
    const auto limits = unity::particleBillboardLimits(params.minParticleSize, params.maxParticleSize,
        params.billboardContextScale, params.cameraHorizontalSpan, 20.f, true);
    volatile float size = startSize * std::max(0.f, curve);
    const auto half = unity::particleBillboardHalfExtents(size, size, limits[2], limits[3], percentAge);
    return double(half[0]) * params.billboardContextScale * params.cameraWorldUnitPx;
}

double subProgress(double lifetimeMs, double tMs)
{
    return clamp(tMs / lifetimeMs, 0, 1);
}

UvTile uvFrameTile(const Subsystem &p, double startFrameNorm, double frameOverTimeValue)
{
    UvTile tile;
    if (!p.uvEnabled || (p.uvTilesX <= 1 && p.uvTilesY <= 1))
        return tile;

    const int tilesX = std::max(1, p.uvTilesX);
    const int tilesY = std::max(1, p.uvTilesY);
    const int totalFrames = tilesX * tilesY;
    const double cycles = std::max(0.001, p.uvCycles);

    // Unity: frame = startFrame + frameOverTime.Evaluate(t) * cycles，再 Repeat 回 [0,1)。
    // frameOverTime 是**帧位置**曲线（0 = 图集第一帧，1 = 绕回第一帧），不是帧速率，
    // 所以这里**不能**再乘一次 progress——调用方传进来的已经是求值结果本身。
    const double frameNorm = startFrameNorm + frameOverTimeValue * cycles;
    const double wrapped = std::fmod(std::fmod(frameNorm, 1.0) + 1.0, 1.0);
    const int frame = static_cast<int>(std::floor(wrapped * totalFrames)) % totalFrames;

    // Unity 图集按行铺，行序自上而下，而 UV 的 v 自下而上，所以行偏移要翻过来。
    const int col = frame % tilesX;
    const int row = frame / tilesX;
    tile.scaleX = 1.0 / tilesX;
    tile.scaleY = 1.0 / tilesY;
    tile.offsetX = col * tile.scaleX;
    tile.offsetY = (tilesY - 1 - row) * tile.scaleY;
    return tile;
}

double subSizeCurve(const Subsystem &p, double progress)
{
    return unity::evaluateParticleCurve(std::span(p.sizeCurve.data, p.sizeCurve.size), float(progress), float(p.sizeCurveMultiplier));
}

Rgba subColor(const Subsystem &p, double progress, double fallbackAlpha)
{
    const std::array<std::uint8_t, 4> start{
        unity::colorByte(float(p.startColor[0])), unity::colorByte(float(p.startColor[1])),
        unity::colorByte(float(p.startColor[2])), unity::colorByte(float(fallbackAlpha))};
    // Unity 先相乘 RGBA8，随后 Apply Active Color Space 解码最终 RGB。
    // 不能分别解码两个输入再乘，也不能省略缓存插值的整数舍入。
    const auto color = p.particleGradient ? unity::lifetimeColor(start, *p.particleGradient,
        std::min(float(clamp(progress, 0.0, 1.0)), std::nextafter(1.f, 0.f))) : start;
    return {srgbToLinearChannel(color[0] / 255.0), srgbToLinearChannel(color[1] / 255.0),
        srgbToLinearChannel(color[2] / 255.0), color[3] / 255.0};
}

} // namespace baclickfx
