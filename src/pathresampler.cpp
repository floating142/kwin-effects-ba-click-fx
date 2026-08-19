// SPDX-License-Identifier: GPL-3.0-or-later

#include "pathresampler.h"

#include <algorithm>
#include <cmath>

namespace baclickfx
{

PathResampleResult resamplePathSegment(const QPointF &from, const QPointF &to,
                                       double spacing, double accumulated)
{
    PathResampleResult result;
    spacing = std::max(spacing, 1e-6);
    accumulated = std::clamp(accumulated, 0.0, spacing);

    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();
    const double distance = std::hypot(dx, dy);
    if (distance <= 0.0) {
        result.remainder = accumulated;
        return result;
    }

    const double dirX = dx / distance;
    const double dirY = dy / distance;
    double walked = 0.0;
    double remaining = distance;
    while (accumulated + remaining >= spacing) {
        const double step = spacing - accumulated;
        walked += step;
        remaining -= step;
        result.points.emplace_back(from.x() + dirX * walked, from.y() + dirY * walked);
        accumulated = 0.0;
    }
    result.remainder = accumulated + remaining;
    return result;
}

} // namespace baclickfx
