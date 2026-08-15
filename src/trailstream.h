// SPDX-License-Identifier: GPL-3.0-or-later
// 拖动 Ribbon 与按距离触发的 Ring4 发射数据。
//
// 轨迹点由 KWin 呈现时钟推进年龄，并在超过 TrailRenderer 寿命后移除。绘制使用
// GPURenderer 的输出级 HDR 目标；轨迹包围盒只参与 damage 计算。

#pragma once

#include "clickinstance.h"
#include "subsystems.h"

#include <QPointF>
#include <QRectF>
#include <cstdint>
#include <vector>

namespace KWin
{

/// 拖尾轨迹采样点；`penUp` 将相邻采样点分隔为不同笔划。
struct TrailPoint
{
    QPointF pos;       // 全局逻辑坐标。
    double age = 0.0;  // 由帧时钟推进的年龄，单位为秒。
    bool penUp = false;
};

/// 管理拖尾轨迹点的追加、断笔与老化。
class TrailStream
{
public:
    /// 开始新笔划，并插入断笔标记。
    void beginStroke(const QPointF &p);

    /// 追加从 `from` 到 `to` 的轨迹段。
    void addSegment(const QPointF &from, const QPointF &to);

    /// 推进轨迹年龄并移除超过 `lifeSec` 的点；有点被移除时返回 `true`。
    bool advance(double dt, double lifeSec);

    /// 返回全部轨迹点向外扩展 `padPx` 后的包围盒。
    QRectF boundingBox(double padPx) const;

    bool empty() const { return m_points.empty(); }
    void clear() { m_points.clear(); }
    const std::vector<TrailPoint> &points() const { return m_points; }

private:
    /// 插入轨迹点，并从头部移除超过容量上限的点。
    void pushPoint(const QPointF &p, bool penUp);

    std::vector<TrailPoint> m_points;
};

/// 单条笔划中转换到输出局部坐标的渲染采样点。
struct StrokeSample
{
    QPointF pos;
    double headT = 0.0;    // 0 表示尾部，1 表示头部。
    double width = 0.0;    // 最终逻辑像素宽度。
    double textureU = 0.0; // TrailRenderer 纹理模式计算出的 U 坐标。
};

struct StrokeData
{
    std::vector<StrokeSample> samples;
    double totalLength = 0.0;
};

/**
 * 按断笔标记将轨迹转换为可渲染笔划。
 *
 * @param stream 轨迹点集。
 * @param trail TrailRenderer 子系统参数。
 * @param origin 输出左上角的全局逻辑坐标。
 * @return 至少包含两个采样点的笔划集合。
 *
 * 宽度与纹理重复间距均从 `trail.worldUnitPx` 派生，其中已包含输出高度和
 * `globalScale` 换算。
 */
std::vector<StrokeData> buildTrailStrokes(const TrailStream &stream,
                                          const baclickfx::Subsystem &trail,
                                          const QPointF &origin);

/// 采样 TrailRenderer 渐变颜色，并返回线性 RGB；`tNorm=0` 为头部。
baclickfx::Rgb evalTrailGradientColor(const baclickfx::Subsystem &trail, double tNorm);

/// 采样 TrailRenderer 渐变透明度；`tNorm=0` 为头部。
double evalTrailGradientAlpha(const baclickfx::Subsystem &trail, double tNorm);

/// 距离发射器产生的一次 Ring4 粒子簇。
struct TriBurstInstance
{
    QPointF center;
    double age = 0.0;
    double life = 0.0;
    double halfSize = 0.0;
    bool drawn = false;
    // Ring4 的 maxNumParticles 作用于单次按下，因此用流水号关联所属拖动会话。
    std::uint64_t dragSerial = 0;
    TriBurstEmission burst;
};

/**
 * 创建一次按距离触发的 Ring4 粒子簇。
 *
 * `countCap < 0` 表示不限制数量；非负值用于执行单次拖动的粒子上限。
 */
TriBurstInstance makeTriBurstInstance(const QPointF &pos, const baclickfx::Subsystem &ring4,
                                      int emitEvents, double timeScale, int countCap,
                                      baclickfx::Rng &rng);

/**
 * 将 Unity `rateOverDistance` 换算为逻辑像素发射间距。
 *
 * 返回 `worldUnitPx / rateOverDistance`；非正发射率返回无穷大。
 */
double distanceStepFromRing4(const baclickfx::Subsystem &ring4);

} // namespace KWin
