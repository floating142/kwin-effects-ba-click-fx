// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "baclickfxdefaults.h"

#include <QJsonObject>
#include <QString>

#include <algorithm>

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

inline double configuredOutputScale(const QJsonObject &overrides, const QString &id,
                                    const QString &fingerprint)
{
    const QJsonValue value = !id.isEmpty() && overrides.contains(id)
        ? overrides.value(id) : overrides.value(fingerprint);
    return std::clamp(value.toDouble(defaults::kGlobalScaleDefault),
                      defaults::kGlobalScaleMin, defaults::kGlobalScaleMax);
}

} // namespace baclickfx
