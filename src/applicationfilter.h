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
    QString resourceName;
    QString displayName;
};

/// 将窗口标识规范化，避免大小写、空白和 .desktop 后缀影响比较。
QString normalizeApplicationId(const QString &value);

/// 规则中记录的每项标识都必须由窗口提供并完全相符。
bool matchesExcludedApplication(const ExcludedApplication &rule,
                                const QString &desktopFile,
                                const QString &resourceClass,
                                const QString &resourceName);
bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const QString &desktopFile,
                           const QString &resourceClass,
                           const QString &resourceName);

bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate);
QVector<ExcludedApplication> excludedApplicationsFromJson(const QJsonArray &array);
QJsonArray excludedApplicationsToJson(const QVector<ExcludedApplication> &rules);
QVector<ExcludedApplication> parseExcludedApplications(const QByteArray &json);
QByteArray serializeExcludedApplications(const QVector<ExcludedApplication> &rules);

} // namespace baclickfx
