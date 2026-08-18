// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRect>
#include <QRectF>

namespace baclickfx
{

QRect quantizeDamageRect(const QRectF &bounds, int tilePx = 64);

} // namespace baclickfx
