// SPDX-License-Identifier: GPL-3.0-or-later

#include "subsystems.h"

#include <algorithm>
#include <cmath>

namespace baclickfx {

using namespace baclickfx::curves;

namespace {

// NaN 表示字段未设置，此时使用对应回退值。
double resolve(double value, double fallback)
{
    return std::isnan(value) ? fallback : value;
}

// JavaScript Math.round 采用 floor(x + 0.5)，其负半数行为与 std::round 不同。
double jsRound(double v)
{
    return std::floor(v + 0.5);
}

// widthCurve 的默认值为恒定 1。
constexpr ScalarStop kDefaultWidthCurve[] = {
    {0.0, 1.0},
};

// 补全可选字段并计算逻辑像素域派生量。
void finalize(Subsystem &s, double ts, double gs, double worldUnitPx)
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

    // localScale 同时作用于尺寸和速度；worldUnitPx 已包含输出高度和 globalScale。
    s.worldUnitPx = worldUnitPx;
    s.localScalePx = s.localScale * worldUnitPx;
    s.sizePx = s.size * s.localScale * worldUnitPx;
    s.speedPxPerSec = s.speed * s.localScale * worldUnitPx;
    s.widthPx = s.widthMultiplier * worldUnitPx;
    s.minVertexDistancePx = s.minVertexDistance * worldUnitPx;
    s.hdrGain = (s.hdrColor[0] + s.hdrColor[1] + s.hdrColor[2]) / 3;
    (void)gs;
}

} // namespace

SubsystemMap buildSubsystemMap(double timeScale, double globalScale, double outputHeightPx)
{
    SubsystemMap m;

    // FX_Touch (PS14) 不在表里：它是预制体根节点，发射模块与渲染器都关着，
    // 材质槽全空，什么都不画。详见 clickinstance.h 顶部的说明。

    // ===== ring (PS15)：点击处的单个扩散圆环 =====
    {
        Subsystem &s = m.ring;
        // PSR_23 → FX_MAT_Touch_Cross2：_Color HDR = 2.0，queue 4499，
        // shader FX_SHADER_AlphaBlend_Add (guid 62e46943)。
        s.hdrColor = Rgb{2.0, 2.0, 2.0};
        s.textureAsset = "FX_TEX_Circle_01.png";
        s.textureClamp = true;
        s.lifetimeSec = 0.2;
        s.size = 0.12;
        s.localScale = 1.0;
        // PS15 InitialModule.startSize (mode=0, scalar=0.12)
        s.startSizeMode = 0;
        s.startSizeScalar = 0.11999999731779099;
        s.startSizeMinScalar = 0.14000000059604645;
        // PS15 InitialModule.startRotation (mode=3, random between 0 and 2PI)
        s.startRotationMode = 3;
        s.startRotationScalar = 6.283185005187988;
        s.startRotationMinScalar = 0.0;
        s.speed = 0.0;
        s.rateOverDistance = 0.0;
        s.clickBurst = 1;
        s.sizeCurve = ScalarCurve(kRingSize);
        s.colorStops = ColorCurve(kRingColor);
        s.alphaStops = ScalarCurve(kRingAlpha);
        s.shapeEnabled = false;
        s.uvEnabled = false;
    }

    // ===== Ring(3) (PS18)：点击迸发的 4 个向外飞散圆环 =====
    {
        Subsystem &s = m.ring3;
        // PSR_20 → FX_MAT_Touch_Tri 2：_Color HDR = 5.992157，queue 3000，
        // shader FX_SHADER_Additive_0 (guid da9b7eae)，Blend One One 纯加法。
        s.hdrColor = Rgb{5.992157, 5.992157, 5.992157};
        // PS18 InitialModule.startColor = (0.5377358, 0.5377358, 0.5377358, 1)。
        // Renderer 启用 Apply Active Color Space；该颜色在 subColor() 中解码。
        s.startColor = Rgb{0.5377358, 0.5377358, 0.5377358};
        s.textureAsset = "FX_TEX_Triangle_02_1.png";
        s.textureClamp = true;
        s.lifetimeSec = 0.6;
        // PS18 InitialModule.startLifetime (mode=3, random between 0.6 and 0.7)
        s.startLifetimeMode = 3;
        s.startLifetimeScalar = 0.6000000238418579;
        s.startLifetimeMinScalar = 0.699999988079071;
        s.size = 0.2;
        // Transform_7.m_LocalScale.x
        s.localScale = 0.3078823983669281;
        // PS18 InitialModule.startSpeed (mode=3, random between 0.3 and 0.4)
        s.startSpeedMode = 3;
        s.startSpeedScalar = 0.4000000059604645;
        s.startSpeedMinScalar = 0.30000001192092896;
        // PS18 InitialModule.startSize (mode=3, random between 0.1 and 0.2)
        s.startSizeMode = 3;
        s.startSizeScalar = 0.20000000298023224;
        s.startSizeMinScalar = 0.10000000149011612;
        s.speed = 0.4;
        s.rateOverDistance = 0.0;
        s.shapeScale = 0.30000001192092896;
        // JSON (PS18) burst scalar=4.
        s.clickBurst = 4;
        s.sizeCurve = ScalarCurve(kRing3Size);
        s.colorStops = ColorCurve(kRing3Color);
        s.alphaStops = ScalarCurve(kRing3Alpha);
        s.shapeEnabled = true;
        s.uvEnabled = true;
        s.uvTilesX = 2;
        s.uvTilesY = 1;
        s.uvCycles = 1.0;
        s.uvStartFrameMode = 3;
        s.uvStartFrameScalar = 0.5;
        s.uvStartFrameMinScalar = 0.0;
        s.uvStartFrame = 0.5;
        s.uvFrameOverTimeMode = 3;
        s.uvFrameOverTimeScalar = 0.5;
        s.uvFrameOverTimeMinScalar = 0.0;
        s.uvFrameOverTime = 0.5;
    }

    // ===== Ring(4) (PS17)：拖动时按距离连发的小圆环 =====
    {
        Subsystem &s = m.ring4;
        // PSR_21 与 PSR_20 共用材质槽 FileID=4，即同一个 FX_MAT_Touch_Tri 2。
        s.hdrColor = Rgb{5.992157, 5.992157, 5.992157};
        // PS17 InitialModule.startColor 与 PS18 同值（同一个材质槽、同一套外观）。
        s.startColor = Rgb{0.5377358, 0.5377358, 0.5377358};
        s.textureAsset = "FX_TEX_Triangle_02_1.png";
        s.textureClamp = true;
        s.lifetimeSec = 0.2;
        // PS17 InitialModule.startLifetime (mode=3, random between 0.2 and 0.4)
        s.startLifetimeMode = 3;
        s.startLifetimeScalar = 0.20000000298023224;
        s.startLifetimeMinScalar = 0.4000000059604645;
        s.size = 0.2;
        // Transform_8.m_LocalScale.x
        s.localScale = 0.3078823983669281;
        // PS17 InitialModule.startSpeed (mode=3, random between 0.2 and 0.3)
        s.startSpeedMode = 3;
        s.startSpeedScalar = 0.20000000298023224;
        s.startSpeedMinScalar = 0.30000001192092896;
        // PS17 InitialModule.startSize (mode=3, random between 0.1 and 0.2)
        s.startSizeMode = 3;
        s.startSizeScalar = 0.20000000298023224;
        s.startSizeMinScalar = 0.10000000149011612;
        s.speed = 0.2;
        s.rateOverDistance = 5.0;
        s.shapeScale = 0.15000000596046448;
        // JSON (PS17) burst count is 0; this system is driven by rateOverDistance.
        s.clickBurst = 0;
        // 每次距离触发对应一次发射事件（基础发射数为 1）。
        s.distanceBurst = 1;
        // PS17 InitialModule.maxNumParticles = 50：单次按下最多同时在场 50 片。
        // 与 ba-click-fx/docs/unity-reference-baseline.md 的「拖尾碎片每次按下
        // 实例最多 50」是同一条合同。执行点在 BaClickFxEffect::updateDrag。
        s.maxParticles = 50;
        s.sizeCurve = ScalarCurve(kRing4Size);
        s.colorStops = ColorCurve(kRing4Color);
        s.alphaStops = ScalarCurve(kRing4Alpha);
        s.shapeEnabled = true;
        s.uvEnabled = true;
        s.uvTilesX = 2;
        s.uvTilesY = 1;
        s.uvCycles = 1.0;
        s.uvStartFrameMode = 3;
        s.uvStartFrameScalar = 0.5;
        s.uvStartFrameMinScalar = 0.0;
        s.uvStartFrame = 0.5;
        s.uvFrameOverTimeMode = 3;
        s.uvFrameOverTimeScalar = 0.5;
        s.uvFrameOverTimeMinScalar = 0.0;
        s.uvFrameOverTime = 0.5;
    }

    // ===== MeshTri (PS16)：点击迸发的 2 个旋转三角 =====
    {
        Subsystem &s = m.meshTri;
        // PSR_22 → FX_MAT_Touch_Tri3，shader FX_SHADER_Dissolve_GachaGauge_P。
        // ShaderGraph 的 HDR 颜色属性 Color_D20A2F66 = 5.992157。
        s.hdrColor = Rgb{5.992157, 5.992157, 5.992157};
        s.textureAsset = "FX_TEX_Grad_Ring3.png";
        s.textureClamp = true;
        s.lifetimeSec = 0.6;
        s.size = 0.12;
        s.localScale = 1.0;
        s.startSpeedMode = 0;
        s.startSpeedScalar = 0.0;
        s.startSpeedMinScalar = 0.0;
        // PS16 InitialModule.startSize (mode=3, random between 0.12 and 0.14)
        s.startSizeMode = 3;
        s.startSizeScalar = 0.11999999731779099;
        s.startSizeMinScalar = 0.14000000059604645;
        // PS16 InitialModule.startRotation (mode=3, random between 0 and 2PI)
        s.startRotationMode = 3;
        s.startRotationScalar = 6.283185005187988;
        s.startRotationMinScalar = 0.0;
        s.speed = 0.0;
        s.rateOverDistance = 0.0;
        s.clickBurst = 2;
        s.rotationEnabled = true;
        s.rotationScalar = 11.170106887817383;
        s.rotationCurveMin = ScalarCurve(kMeshTriRotationMin);
        s.rotationCurveMax = ScalarCurve(kMeshTriRotationMax);
        // PS16 CustomDataModule.vector0_0.maxCurve: 1 -> 0 -> 1.
        s.customData0Curve = ScalarCurve(kMeshTriCustomData0);
        s.sizeCurve = ScalarCurve(kMeshTriSize);
        s.colorStops = ColorCurve(kMeshTriColor);
        s.alphaStops = ScalarCurve(kMeshTriAlpha);
        s.shapeEnabled = false;
        s.uvEnabled = false;
    }

    // ===== Trail (TrailRenderer_13)：拖动时的 ribbon 拖尾 =====
    {
        Subsystem &s = m.trail;
        // TrailRenderer_13.m_Time
        s.lifetimeSec = 0.30000001192092896;
        // TrailRenderer_13.m_Parameters.widthMultiplier
        s.widthMultiplier = 0.004999999888241291;
        // TrailRenderer_13.m_Parameters.widthCurve
        s.widthCurve = ScalarCurve(kTrailWidth);
        // TrailRenderer_13.m_MinVertexDistance
        s.minVertexDistance = 0.009999999776482582;
        // TrailRenderer_13.m_Parameters.numCornerVertices / numCapVertices
        s.numCornerVertices = 4;
        s.numCapVertices = 1;
        // TrailRenderer_13.textureMode / textureScale
        s.textureMode = 0;
        // 序列化数据未保存 textureScale，因此使用 Unity 默认值 1。
        s.textureScale = 1.0;
        // FxTrailTimeScale.killUnderTimeScale
        s.killUnderTimeScale = 0.19;
        // FX_MAT_TouchFXTrail.mat _Color (HDR)
        s.hdrColor = Rgb{23.968628, 23.968628, 23.968628};
        // _Texture = FX_TEX_Trail_03.png（Stretch + Clamp 语义近似）
        s.textureAsset = "FX_TEX_Trail_03.png";
        s.textureClamp = true;
        // TrailRenderer_13.m_Parameters.colorGradient
        s.gradientMode = 0;
        s.colorKeys = ColorCurve(kTrailColorKeys);
        s.alphaKeys = ScalarCurve(kTrailAlphaKeys);
    }

    // Unity 正交相机的垂直范围恒为 2 个世界单位（orthographicSize = 1.0），
    // 所以 1 世界单位 = 输出逻辑高度的一半。kUnitySizeToPx(540) 是参考高度
    // 1080 下的值，这里再乘 outputHeight/1080 归一化 —— 两者约掉后就是
    // outputHeight/2，特效在 1080p / 1440p / 4K 上占屏比例完全一致。
    const double height = outputHeightPx > 0.0 ? outputHeightPx : kReferenceHeightPx;
    const double worldUnitPx = kUnitySizeToPx * (height / kReferenceHeightPx) * globalScale;

    for (Subsystem *s : {&m.ring, &m.ring3, &m.ring4, &m.meshTri, &m.trail})
        finalize(*s, timeScale, globalScale, worldUnitPx);

    return m;
}

double subProgress(double lifetimeMs, double tMs)
{
    return clamp(tMs / lifetimeMs, 0, 1);
}

double sampleMinMaxConstant(int mode, double scalar, double minScalar, Rng &rng)
{
    if (mode == 3) {
        const double lo = std::min(minScalar, scalar);
        const double hi = std::max(minScalar, scalar);
        return rng.range(lo, hi);
    }

    return scalar;
}

double sampleStartLifetimeSec(const Subsystem &p, Rng &rng)
{
    const double sampled = sampleMinMaxConstant(
        p.startLifetimeMode, p.startLifetimeScalar, p.startLifetimeMinScalar, rng);
    return std::max(0.0001, sampled);
}

double sampleStartSizeMul(const Subsystem &p, Rng &rng)
{
    const double sampled = sampleMinMaxConstant(
        p.startSizeMode, p.startSizeScalar, p.startSizeMinScalar, rng);
    return sampled / std::max(1e-6, p.size);
}

double sampleStartRotation(const Subsystem &p, Rng &rng)
{
    return sampleMinMaxConstant(
        p.startRotationMode, p.startRotationScalar, p.startRotationMinScalar, rng);
}

double sampleStartSpeedPx(const Subsystem &p, Rng &rng)
{
    return sampleMinMaxConstant(
        p.startSpeedMode, p.startSpeedScalar, p.startSpeedMinScalar, rng);
}

double sampleUvStartFrameNorm(const Subsystem &p, Rng &rng)
{
    const double sampled = sampleMinMaxConstant(
        p.uvStartFrameMode, p.uvStartFrameScalar, p.uvStartFrameMinScalar, rng);
    return clamp(sampled, 0, 1);
}

double sampleUvFrameOverTimeNorm(const Subsystem &p, Rng &rng)
{
    const double sampled = sampleMinMaxConstant(
        p.uvFrameOverTimeMode, p.uvFrameOverTimeScalar, p.uvFrameOverTimeMinScalar, rng);
    return std::max(0.0, sampled);
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
    return evalScalarStops(p.sizeCurve, progress, 1.0);
}

Rgba subColor(const Subsystem &p, double progress, double fallbackAlpha)
{
    // 空颜色曲线回退到纯黑，使缺失参数表现为明确的无颜色输出。
    const Rgb rgb = evalColorStops(p.colorStops, progress, Rgb{0.0, 0.0, 0.0});
    const double alpha = evalScalarStops(p.alphaStops, progress, fallbackAlpha);

    // Unity 工程是 Linear 颜色空间：Gradient / Color 字段存的是 sRGB 编码值，
    // 运行时先解码到线性再进混合。curvedata.h 里的关键帧是从序列化资源原样抄来的，
    // 所以也是 sRGB 编码的，必须在这里解码——否则等于整体做了一次 gamma 提亮。
    // 插值仍在 sRGB 域完成（与 Unity 的 Gradient.Evaluate 一致），解码放在插值之后。
    //
    // startColor 同理：Renderer 勾了 Apply Active Color Space，它是**线性空间的
    // 乘子**，不是显示空间的。0.5377358 解码后 ≈ 0.2506。
    const Rgb sc{srgbToLinearChannel(p.startColor[0]),
                 srgbToLinearChannel(p.startColor[1]),
                 srgbToLinearChannel(p.startColor[2])};
    return Rgba{srgbToLinearChannel(rgb[0]) * sc[0],
                srgbToLinearChannel(rgb[1]) * sc[1],
                srgbToLinearChannel(rgb[2]) * sc[2],
                alpha};
}

} // namespace baclickfx
