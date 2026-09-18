// SPDX-License-Identifier: GPL-3.0-or-later

#include "applicationfilter.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace baclickfx
{

QString normalizeApplicationId(const QString &value)
{
    QString normalized = value.trimmed().toCaseFolded();
    if (normalized.endsWith(QLatin1String(".desktop"))) {
        normalized.chop(8);
    }
    return normalized;
}

bool matchesExcludedApplication(const ExcludedApplication &rule,
                                const QString &desktopFile,
                                const QString &resourceClass)
{
    const QString actualDesktopFile = normalizeApplicationId(desktopFile);
    if (!rule.desktopFile.isEmpty() && !actualDesktopFile.isEmpty()) {
        return rule.desktopFile == actualDesktopFile;
    }

    const QString actualResourceClass = normalizeApplicationId(resourceClass);
    return !rule.resourceClass.isEmpty()
        && !actualResourceClass.isEmpty()
        && rule.resourceClass == actualResourceClass;
}

bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const QString &desktopFile,
                           const QString &resourceClass)
{
    const QString actualDesktopFile = normalizeApplicationId(desktopFile);
    const QString actualResourceClass = normalizeApplicationId(resourceClass);
    for (const ExcludedApplication &rule : rules) {
        if (!rule.desktopFile.isEmpty() && !actualDesktopFile.isEmpty()) {
            if (rule.desktopFile == actualDesktopFile) {
                return true;
            }
            continue;
        }
        if (!rule.resourceClass.isEmpty()
            && !actualResourceClass.isEmpty()
            && rule.resourceClass == actualResourceClass) {
            return true;
        }
    }
    return false;
}

bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate)
{
    for (const ExcludedApplication &rule : rules) {
        if (matchesExcludedApplication(rule, candidate.desktopFile, candidate.resourceClass)
            || matchesExcludedApplication(candidate, rule.desktopFile, rule.resourceClass)) {
            return true;
        }
    }
    return false;
}

QVector<ExcludedApplication> excludedApplicationsFromJson(const QJsonArray &array)
{
    QVector<ExcludedApplication> result;
    result.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        ExcludedApplication application{
            .desktopFile = normalizeApplicationId(
                object.value(QStringLiteral("desktopFile")).toString()),
            .resourceClass = normalizeApplicationId(
                object.value(QStringLiteral("resourceClass")).toString()),
            .displayName = object.value(QStringLiteral("displayName")).toString().trimmed(),
        };
        if ((application.desktopFile.isEmpty() && application.resourceClass.isEmpty())
            || containsExcludedApplication(result, application)) {
            continue;
        }
        result.push_back(std::move(application));
    }
    return result;
}

QJsonArray excludedApplicationsToJson(const QVector<ExcludedApplication> &rules)
{
    QJsonArray array;
    QVector<ExcludedApplication> unique;
    unique.reserve(rules.size());
    for (const ExcludedApplication &raw : rules) {
        ExcludedApplication rule{
            .desktopFile = normalizeApplicationId(raw.desktopFile),
            .resourceClass = normalizeApplicationId(raw.resourceClass),
            .displayName = raw.displayName.trimmed(),
        };
        if ((rule.desktopFile.isEmpty() && rule.resourceClass.isEmpty())
            || containsExcludedApplication(unique, rule)) {
            continue;
        }
        unique.push_back(rule);

        QJsonObject object;
        if (!rule.desktopFile.isEmpty()) {
            object.insert(QStringLiteral("desktopFile"), rule.desktopFile);
        }
        if (!rule.resourceClass.isEmpty()) {
            object.insert(QStringLiteral("resourceClass"), rule.resourceClass);
        }
        if (!rule.displayName.isEmpty()) {
            object.insert(QStringLiteral("displayName"), rule.displayName);
        }
        array.append(object);
    }
    return array;
}

QVector<ExcludedApplication> parseExcludedApplications(const QByteArray &json)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    return document.isArray() ? excludedApplicationsFromJson(document.array())
                              : QVector<ExcludedApplication>();
}

QByteArray serializeExcludedApplications(const QVector<ExcludedApplication> &rules)
{
    return QJsonDocument(excludedApplicationsToJson(rules)).toJson(QJsonDocument::Compact);
}

} // namespace baclickfx
