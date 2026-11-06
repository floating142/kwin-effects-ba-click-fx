// SPDX-License-Identifier: GPL-3.0-or-later

#include "trailstream.h"
#include "curveutils.h"

#include <QLineF>

#include <algorithm>
#include <cmath>

namespace KWin
{

using baclickfx::Rgb;
using baclickfx::clamp;

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
    if (!trail.particleGradient || !trail.particleGradient->enabled) return 1;
    return trail.particleGradient->evaluateBytes(float(clamp(tNorm, 0, 1)))[3] / 255.0;
}

Rgb evalTrailGradientColor(const baclickfx::Subsystem &trail, double tNorm)
{
    if (!trail.particleGradient || !trail.particleGradient->enabled) return {0, .3905813694000244, 1};
    // 原生 LineRenderer 先沿长度求 Gradient，再打包 RGBA8 顶点色。
    const auto bytes = trail.particleGradient->evaluateBytes(float(clamp(tNorm, 0, 1)));
    // Trail 顶点色由原 Forward 顶点着色器直接透传；它没有粒子系统的
    // ApplyActiveColorSpace 步骤，不能复用 ParticleSystem 的 sRGB 解码。
    return {bytes[0] / 255.0, bytes[1] / 255.0, bytes[2] / 255.0};
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
        data.totalLength = 0.0;
        const std::size_t count = strokeEnd - strokeStart;
        data.sourcePoints.resize(count);
        data.preparedPoints.resize(count);
        data.emissionFlags.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &point = pts[strokeStart + i];
            data.sourcePoints[i] = {{float(point.pos.x()), float(point.pos.y()), 0.f},
                                    point.birthTime, 1};
        }
        const auto liveHead = strokeEnd == pts.size() ? stream.head() : std::nullopt;
        const QPointF head = liveHead.value_or(pts[strokeEnd - 1].pos);
        const auto worldUnitAt = [&](std::size_t i) {
            const double unit = liveHead && i + 1 == count
                ? stream.headWorldUnitPx() : pts[strokeStart + i].worldUnitPx;
            return unit > 0 ? unit : trail.worldUnitPx;
        };
        baclickfx::unity::prepareTrailPoints(data.sourcePoints, 0, count,
            {float(head.x()), float(head.y()), 0.f}, true, stream.time(), stream.lifetime(),
            data.preparedPoints, data.emissionFlags);
        const auto position = [&data, count](std::size_t i) {
            const auto &p = data.preparedPoints[count - 1 - i];
            return QPointF(p[0], p[1]);
        };
        // 原生点准备结果为头到尾；现有几何构造按尾到头读取。
        data.segmentLengths.reserve(count - 1);
        for (std::size_t i = 1; i < count; ++i) {
            const double length = QLineF(position(i - 1), position(i)).length();
            data.segmentLengths.push_back(length);
            data.totalLength += length;
        }
        data.samples.reserve(count);
        double pathLength = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const double geometryT = data.totalLength > 1e-6 ? pathLength / data.totalLength : 0;
            const double headT = 1.0 - clamp(geometryT, 0, 1);
            StrokeSample sample;
            sample.pos = position(i);
            sample.headT = headT;
            const double pointUnit = worldUnitAt(i);
            const double pointWidth = pointUnit == trail.worldUnitPx
                ? baseWidth : std::max(0.01, trail.widthMultiplier * pointUnit);
            sample.width = pointWidth * sampleTrailWidthFactor(trail, headT);
            sample.textureU = 1.0 - sampleTrailTextureU(trail, pathLength, data.totalLength);
            sample.alpha = evalTrailGradientAlpha(trail, headT);
            sample.color = evalTrailGradientColor(trail, headT);
            data.samples.push_back(sample);
            if (i + 1 < count) pathLength += data.segmentLengths[i];
        }

        if (data.samples.size() >= 2) {
            data.ribbonOrigin = head;
            const double unit = worldUnitAt(count - 1);
            data.worldUnitPx = unit;
            data.ribbonInput.resize(count);
            bool mixedProjection = false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto &p = data.preparedPoints[i];
                data.ribbonInput[i] = {float((double(p[0]) - head.x()) / unit),
                                      -float((double(p[1]) - head.y()) / unit)};
                mixedProjection |= worldUnitAt(count - 1 - i) != unit;
            }
            baclickfx::unity::buildLineRibbon(data.ribbonInput, float(trail.widthMultiplier),
                trail.numCornerVertices, trail.numCapVertices, data.ribbonScratch, data.ribbon);
            // 跨屏时按采样点的倍率调整相对中心线的宽度，保持笔划与渐变连续。
            if (mixedProjection) {
                for (auto &vertex : data.ribbon) {
                    const double pointUnit = worldUnitAt(count - 1 - vertex.sample);
                    if (pointUnit == unit) {
                        continue;
                    }
                    const auto &center = data.ribbonInput[vertex.sample];
                    for (std::size_t axis = 0; axis < 2; ++axis) {
                        const double offset = double(vertex.position[axis]) - center[axis];
                        vertex.position[axis] = float(center[axis] + offset * (pointUnit / unit));
                    }
                }
            }
            for (std::size_t i = 0; i < count; ++i) {
                auto &sample = data.samples[count - 1 - i];
                sample.headT = data.ribbonScratch.progress[i];
                sample.textureU = sample.headT;
                sample.color = evalTrailGradientColor(trail, sample.headT);
                sample.alpha = evalTrailGradientAlpha(trail, sample.headT);
            }
        } else {
            outputCount--;
        }
        strokeStart = strokeEnd;
    }
    out.resize(outputCount);
}

// TrailStream

void TrailStream::pushPoint(const QPointF &p, bool penUp, double worldUnitPx)
{
    m_points.push_back(TrailPoint{p, float(m_time), penUp, worldUnitPx});
    setHead(p, worldUnitPx);
    if (m_points.size() > kMaxPoints) {
        m_points.pop_front();
    }
}

void TrailStream::beginStroke(const QPointF &p, double worldUnitPx)
{
    pushPoint(p, true, worldUnitPx);
}

void TrailStream::addSegment(const QPointF &from, const QPointF &to, double worldUnitPx)
{
    if (m_points.empty()) {
        pushPoint(from, true, worldUnitPx);
    }
    pushPoint(to, false, worldUnitPx);
}

bool TrailStream::retireExpired(double lifeSec)
{
    m_lifetime = float(lifeSec);
    const std::size_t before = m_points.size();
    const std::size_t firstAlive = baclickfx::unity::expiredTrailPrefix(
        before, time(), m_lifetime, [this](std::size_t i) { return m_points[i].birthTime; });
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

    if (m_head) {
        minX = std::min(minX, m_head->x());
        maxX = std::max(maxX, m_head->x());
        minY = std::min(minY, m_head->y());
        maxY = std::max(maxY, m_head->y());
    }
    return QRectF(minX - padPx, minY - padPx,
                  (maxX - minX) + padPx * 2, (maxY - minY) + padPx * 2);
}

} // namespace KWin
