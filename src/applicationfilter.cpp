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
                                const QString &resourceClass,
                                const QString &resourceName)
{
    const QString actualDesktopFile = normalizeApplicationId(desktopFile);
    const QString actualResourceClass = normalizeApplicationId(resourceClass);
    const QString actualResourceName = normalizeApplicationId(resourceName);

    bool hasIdentity = false;
    const auto compareField = [&hasIdentity](const QString &expected, const QString &actual) {
        if (expected.isEmpty()) {
            return true;
        }
        hasIdentity = true;
        return !actual.isEmpty() && expected == actual;
    };
    return compareField(rule.desktopFile, actualDesktopFile)
        && compareField(rule.resourceClass, actualResourceClass)
        && compareField(rule.resourceName, actualResourceName)
        && hasIdentity;
}

bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const QString &desktopFile,
                           const QString &resourceClass,
                           const QString &resourceName)
{
    for (const ExcludedApplication &rule : rules) {
        if (matchesExcludedApplication(rule, desktopFile, resourceClass, resourceName)) {
            return true;
        }
    }
    return false;
}

bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate)
{
    for (const ExcludedApplication &rule : rules) {
        if (rule.desktopFile == candidate.desktopFile
            && rule.resourceClass == candidate.resourceClass
            && rule.resourceName == candidate.resourceName) {
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
            .resourceName = normalizeApplicationId(
                object.value(QStringLiteral("resourceName")).toString()),
            .displayName = object.value(QStringLiteral("displayName")).toString().trimmed(),
        };
        if ((application.desktopFile.isEmpty() && application.resourceClass.isEmpty()
             && application.resourceName.isEmpty())
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
            .resourceName = normalizeApplicationId(raw.resourceName),
            .displayName = raw.displayName.trimmed(),
        };
        if ((rule.desktopFile.isEmpty() && rule.resourceClass.isEmpty()
             && rule.resourceName.isEmpty())
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
        if (!rule.resourceName.isEmpty()) {
            object.insert(QStringLiteral("resourceName"), rule.resourceName);
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
