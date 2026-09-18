// SPDX-License-Identifier: GPL-3.0-or-later
// 应用排除规则的稳定序列化与匹配逻辑。

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <QVector>

namespace baclickfx
{

struct ExcludedApplication {
    QString desktopFile;
    QString resourceClass;
    QString displayName;
};

/// 将 desktop-file ID 或窗口类名转成用于比较的规范形式。
QString normalizeApplicationId(const QString &value);

/// 优先比较 desktop-file ID；它不可用时才回退到窗口类名。
bool matchesExcludedApplication(const ExcludedApplication &rule,
                                const QString &desktopFile,
                                const QString &resourceClass);
bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const QString &desktopFile,
                           const QString &resourceClass);

bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate);
QVector<ExcludedApplication> excludedApplicationsFromJson(const QJsonArray &array);
QJsonArray excludedApplicationsToJson(const QVector<ExcludedApplication> &rules);
QVector<ExcludedApplication> parseExcludedApplications(const QByteArray &json);
QByteArray serializeExcludedApplications(const QVector<ExcludedApplication> &rules);

} // namespace baclickfx
