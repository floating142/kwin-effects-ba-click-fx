// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPointF>

#include <vector>

namespace baclickfx
{

struct PathResampleResult
{
    std::vector<QPointF> points;
    double remainder = 0.0;
};

/// 沿 `from` 到 `to` 按固定累计距离生成采样点，并返回未达到下一间距的余量。
PathResampleResult resamplePathSegment(const QPointF &from, const QPointF &to,
                                       double spacing, double accumulated);

} // namespace baclickfx
