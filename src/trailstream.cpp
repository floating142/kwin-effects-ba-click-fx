// SPDX-License-Identifier: GPL-3.0-or-later

#include "trailstream.h"
#include "curveutils.h"

#include <QLineF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace KWin
{

using baclickfx::Rgb;
using baclickfx::clamp;
using baclickfx::jsRound;

namespace
{

// 实现级容量上限，用于约束内存和绘制开销。1000 点可覆盖 0.3 秒内约 1800px 路程。
constexpr std::size_t kMaxPoints = 1000;

// 按 TrailRenderer.textureMode 和 textureScale 计算纹理 U 坐标。
double sampleTrailTextureU(const baclickfx::Subsystem &trail, double distancePx,
                           double totalLengthPx)
{
    // worldUnitPx 已包含输出高度归一化和 globalScale。
    const double worldUnitPx = std::max(1e-3, trail.worldUnitPx);

    // mode 0 = Stretch：整条轨迹铺满一次贴图；其余按世界单位重复。
    double u = trail.textureMode == 0
        ? (totalLengthPx > 1e-6 ? distancePx / totalLengthPx : 0.0)
        : distancePx / worldUnitPx;
    u *= trail.textureScale;

    const auto wrap = [](double v) {
        v = std::fmod(v, 1.0);
        return v < 0 ? v + 1.0 : v;
    };

    if (trail.textureMode != 0) {
        return wrap(u);
    }
    return trail.textureClamp ? clamp(u, 0, 1) : wrap(u);
}

// 采样 TrailRenderer.widthCurve 的宽度倍率。
double sampleTrailWidthFactor(const baclickfx::Subsystem &trail, double tNorm)
{
    if (trail.widthCurve.empty()) {
        return 1.0;
    }
    return baclickfx::evalScalarStops(trail.widthCurve, clamp(tNorm, 0, 1),
                                    trail.widthCurve.front().v);
}

} // namespace

// 拖尾曲线采样

double evalTrailGradientAlpha(const baclickfx::Subsystem &trail, double tNorm)
{
    const double t = clamp(tNorm, 0, 1);
    if (trail.alphaKeys.empty()) {
        return 1.0;
    }

    // mode 1 是 Unity 的 Fixed 渐变：不插值，取最后一个不晚于 t 的键。
    if (trail.gradientMode == 1) {
        double out = trail.alphaKeys.front().v;
        for (std::size_t i = 1; i < trail.alphaKeys.size; i++) {
            if (trail.alphaKeys[i].t > t) {
                break;
            }
            out = trail.alphaKeys[i].v;
        }
        return out;
    }

    return baclickfx::evalScalarStops(trail.alphaKeys, t, trail.alphaKeys.front().v);
}

Rgb evalTrailGradientColor(const baclickfx::Subsystem &trail, double tNorm)
{
    // TrailRenderer.colorGradient 沿轨迹求值：
    // t=0 在笔头（最新的点），t=1 在笔尾。kTrailColorKeys 从亮蓝
    // (0, 0.3906, 1) 一路压到纯黑，所以正确的观感是"头亮尾暗"。
    const double t = clamp(tNorm, 0, 1);
    if (trail.colorKeys.empty()) {
        return Rgb{0.0, 0.3905813694000244, 1.0};
    }

    Rgb rgb;
    if (trail.gradientMode == 1) {
        // mode 1 是 Unity 的 Fixed 渐变：不插值，取最后一个不晚于 t 的键。
        // 与 evalTrailGradientAlpha 的同名分支保持一致。
        rgb = trail.colorKeys.front().c;
        for (std::size_t i = 1; i < trail.colorKeys.size; i++) {
            if (trail.colorKeys[i].t > t) {
                break;
            }
            rgb = trail.colorKeys[i].c;
        }
    } else {
        rgb = baclickfx::evalColorStops(trail.colorKeys, t, trail.colorKeys.front().c);
    }

    // 与 subColor 一致：关键帧是 sRGB 编码的，插值完再解码到线性。
    return Rgb{baclickfx::srgbToLinearChannel(rgb[0]),
               baclickfx::srgbToLinearChannel(rgb[1]),
               baclickfx::srgbToLinearChannel(rgb[2])};
}

std::vector<StrokeData> buildTrailStrokes(const TrailStream &stream,
                                          const baclickfx::Subsystem &trail)
{
    std::vector<StrokeData> out;
    buildTrailStrokes(stream, trail, out);
    return out;
}

void buildTrailStrokes(const TrailStream &stream, const baclickfx::Subsystem &trail,
                       std::vector<StrokeData> &out)
{
    std::size_t outputCount = 0;
    const std::deque<TrailPoint> &pts = stream.points();
    if (pts.size() < 2) {
        out.clear();
        return;
    }

    const double baseWidth = std::max(0.01, trail.widthPx);

    // 按 penUp 标记切分独立笔划。
    std::size_t strokeStart = 0;
    while (strokeStart + 1 < pts.size()) {
        // 连续的断笔点之间没有内容，跳过。
        while (strokeStart + 1 < pts.size() && pts[strokeStart].penUp
               && pts[strokeStart + 1].penUp) {
            strokeStart++;
        }

        std::size_t strokeEnd = strokeStart + 1;
        while (strokeEnd < pts.size() && !pts[strokeEnd].penUp) {
            strokeEnd++;
        }

        if (outputCount >= out.size()) {
            out.emplace_back();
        }
        StrokeData &data = out[outputCount++];
        data.samples.clear();
        data.segmentLengths.clear();
        data.directions.clear();
        data.normals.clear();
        data.totalLength = 0.0;
        // 先累计整段长度，再按长度归一化采样宽度和颜色。
        data.segmentLengths.reserve(strokeEnd - strokeStart - 1);
        for (std::size_t i = strokeStart + 1; i < strokeEnd; i++) {
            const double length = QLineF(pts[i - 1].pos, pts[i].pos).length();
            data.segmentLengths.push_back(length);
            data.totalLength += length;
        }

        data.samples.reserve(strokeEnd - strokeStart);
        double pathLength = 0;
        for (std::size_t i = strokeStart; i < strokeEnd; i++) {
            const double geometryT = data.totalLength > 1e-6 ? pathLength / data.totalLength : 0;
            const double headT = 1.0 - clamp(geometryT, 0, 1);
            // 贴图 U 反过来取：pathLength=0 那端对应贴图右侧。这个 U 会直接传给
            // shader，采样原始拖尾贴图。
            const double texU =
                1.0 - sampleTrailTextureU(trail, pathLength, data.totalLength);

            StrokeSample s;
            s.pos = pts[i].pos;
            s.headT = headT;
            s.width = baseWidth * sampleTrailWidthFactor(trail, headT);
            s.textureU = texU;
            s.alpha = evalTrailGradientAlpha(trail, headT);
            s.color = evalTrailGradientColor(trail, headT);
            data.samples.push_back(s);

            if (i + 1 < strokeEnd) {
                pathLength += data.segmentLengths[i - strokeStart];
            }
        }

        if (data.samples.size() >= 2) {
            const std::size_t n = data.samples.size();
            data.directions.resize(n - 1);
            data.normals.resize(n);
            QPointF lastDir(1, 0);
            for (std::size_t i = 0; i + 1 < n; i++) {
                const double dx = data.samples[i + 1].pos.x() - data.samples[i].pos.x();
                const double dy = data.samples[i + 1].pos.y() - data.samples[i].pos.y();
                const double len = std::hypot(dx, dy);
                if (len > 1e-6) {
                    lastDir = QPointF(dx / len, dy / len);
                }
                data.directions[i] = lastDir;
            }
            for (std::size_t i = 0; i < n; i++) {
                const QPointF dIn = data.directions[i == 0 ? 0 : i - 1];
                const QPointF dOut = data.directions[std::min(data.directions.size() - 1, i)];
                const double ax = -dIn.y() - dOut.y();
                const double ay = dIn.x() + dOut.x();
                const double inv = 1.0 / std::max(1e-6, std::hypot(ax, ay));
                data.normals[i] = QPointF(ax * inv, ay * inv);
            }
            // Keep the StrokeData and its sample capacity for the next output frame.
        } else {
            outputCount--;
        }
        strokeStart = strokeEnd;
    }
    out.resize(outputCount);
}

// TrailStream

void TrailStream::pushPoint(const QPointF &p, bool penUp)
{
    m_points.push_back(TrailPoint{p, 0.0, penUp});
    if (m_points.size() > kMaxPoints) {
        m_points.pop_front();
    }
}

void TrailStream::beginStroke(const QPointF &p)
{
    pushPoint(p, true);
}

void TrailStream::addSegment(const QPointF &from, const QPointF &to)
{
    if (m_points.empty()) {
        pushPoint(from, true);
    }
    pushPoint(to, false);
}

bool TrailStream::advance(double dt, double lifeSec)
{
    for (TrailPoint &pt : m_points) {
        pt.age += dt;
    }

    // 点按时间顺序排列，过期的一定在最前面。
    const std::size_t before = m_points.size();
    std::size_t firstAlive = 0;
    while (firstAlive < m_points.size() && m_points[firstAlive].age > lifeSec) {
        firstAlive++;
    }
    if (firstAlive > 0) {
        m_points.erase(m_points.begin(), m_points.begin() + firstAlive);
    }

    return m_points.size() != before;
}

QRectF TrailStream::boundingBox(double padPx) const
{
    if (m_points.empty()) {
        return QRectF();
    }

    double minX = m_points[0].pos.x();
    double maxX = minX;
    double minY = m_points[0].pos.y();
    double maxY = minY;
    for (const TrailPoint &pt : m_points) {
        minX = std::min(minX, pt.pos.x());
        maxX = std::max(maxX, pt.pos.x());
        minY = std::min(minY, pt.pos.y());
        maxY = std::max(maxY, pt.pos.y());
    }

    return QRectF(minX - padPx, minY - padPx,
                  (maxX - minX) + padPx * 2, (maxY - minY) + padPx * 2);
}


// 距离发射器

TriBurstInstance makeTriBurstInstance(const QPointF &pos, const baclickfx::Subsystem &ring4,
                                      int emitEvents, double timeScale, int countCap,
                                      baclickfx::Rng &rng)
{
    TriBurstInstance inst;
    inst.center = pos;
    // 包围盒半径取 1 个 Unity 世界单位。
    inst.halfSize = jsRound(2.0 * ring4.worldUnitPx) / 2;

    // Unity 的 rateOverDistance 是「每单位距离发射多少个」，一帧内可能攒够多次，
    // 所以这里按累计的触发次数放大 burst 数量，而不是丢掉多余的。
    int burstCount =
        std::max(1, static_cast<int>(jsRound(static_cast<double>(ring4.distanceBurst) * emitEvents)));
    // countCap < 0 表示不限；否则按 Ring (4) 的 maxNumParticles 收口（见调用方）。
    if (countCap >= 0) {
        burstCount = std::min(burstCount, countCap);
    }
    inst.burst = emitTriBurst(ring4, timeScale, burstCount, rng);
    // 与粒子进度共用精确 durationSec，避免毫秒四舍五入提前截掉最后一小段。
    inst.life = inst.burst.durationSec;
    return inst;
}

double distanceStepFromRing4(const baclickfx::Subsystem &ring4)
{
    if (ring4.rateOverDistance <= 0) {
        return std::numeric_limits<double>::infinity();
    }

    // Unity 的 rateOverDistance 单位是「发射器每移动一个世界单位发射几个」，
    // 于是间距 = 1/rateOverDistance 个世界单位。再用子系统派生的
    // worldUnitPx（= 输出高度/2 × globalScale）换到像素：
    //
    //     1080p、globalScale=1 时 540 / 5.0 = 108 逻辑像素一个
    //
    // worldUnitPx 里已经含 globalScale：整套特效放大时，发射器在世界空间里走过的
    // "距离"也按同一比例缩放，否则放大后三角密度会跟着变，比例关系就跑了。
    //
    // rateOverDistance 是 PS17 的固定参数。非正值已在上方转换为无穷大；其余值
    // 保持原始单位换算，避免隐藏参数错误。
    return ring4.worldUnitPx / ring4.rateOverDistance;
}

} // namespace KWin
