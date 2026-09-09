// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace baclickfx {

inline QString outputScaleId(const QString &manufacturer, const QString &model,
                             const QString &serialNumber, const QString &name)
{
    return QStringLiteral("%1|%2|%3|%4").arg(manufacturer, model, serialNumber, name);
}

inline QString preferredOutputScaleId(const QString &uuid, const QString &manufacturer,
                                      const QString &model, const QString &serialNumber,
                                      const QString &name)
{
    return uuid.isEmpty() ? outputScaleId(manufacturer, model, serialNumber, name) : uuid;
}

} // namespace baclickfx
