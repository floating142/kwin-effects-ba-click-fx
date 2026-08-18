// SPDX-License-Identifier: GPL-3.0-or-later

#include "damageutils.h"

#include <algorithm>
#include <cmath>

namespace baclickfx
{

QRect quantizeDamageRect(const QRectF &bounds, int tilePx)
{
    if (bounds.isNull() || bounds.isEmpty() || tilePx <= 0) {
        return QRect();
    }
    const int left = int(std::floor(bounds.left() / tilePx)) * tilePx;
    const int top = int(std::floor(bounds.top() / tilePx)) * tilePx;
    const int right = int(std::ceil(bounds.right() / tilePx)) * tilePx;
    const int bottom = int(std::ceil(bounds.bottom() / tilePx)) * tilePx;
    return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

} // namespace baclickfx
