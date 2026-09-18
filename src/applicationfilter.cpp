// SPDX-License-Identifier: GPL-3.0-or-later

#include "applicationfilter.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace baclickfx
{

namespace
{

QString normalizeProcessCommand(QString value)
{
    value = value.trimmed();
    value.replace(u'\\', u'/');
    return value.toCaseFolded();
}

QString normalizeWinePrefix(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QString() : QDir::cleanPath(trimmed);
}

} // namespace

QString normalizeApplicationId(const QString &value)
{
    QString normalized = value.trimmed().toCaseFolded();
    if (normalized.endsWith(QLatin1String(".desktop"))) {
        normalized.chop(8);
    }
    return normalized;
}

ProcessIdentity processIdentity(qint64 pid)
{
    ProcessIdentity identity;
    if (pid <= 0) {
        return identity;
    }

    QFile commandLine(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (commandLine.open(QIODevice::ReadOnly)) {
        const QByteArray firstArgument = commandLine.readAll().split('\0').value(0);
        identity.command = normalizeProcessCommand(QString::fromLocal8Bit(firstArgument));
    }

    QFile environment(QStringLiteral("/proc/%1/environ").arg(pid));
    if (environment.open(QIODevice::ReadOnly)) {
        constexpr auto prefix = "WINEPREFIX=";
        constexpr qsizetype prefixLength = 11;
        const QList<QByteArray> entries = environment.readAll().split('\0');
        for (const QByteArray &entry : entries) {
            if (entry.startsWith(prefix)) {
                identity.winePrefix = normalizeWinePrefix(
                    QString::fromLocal8Bit(entry.sliced(prefixLength)));
                break;
            }
        }
    }
    return identity;
}

bool matchesExcludedApplication(const ExcludedApplication &rule,
                                const QString &desktopFile,
                                const QString &resourceClass,
                                const QString &resourceName,
                                const QString &processCommand,
                                const QString &winePrefix)
{
    const QString actualDesktopFile = normalizeApplicationId(desktopFile);
    const QString actualResourceClass = normalizeApplicationId(resourceClass);
    const QString actualResourceName = normalizeApplicationId(resourceName);
    const QString actualProcessCommand = normalizeProcessCommand(processCommand);
    const QString actualWinePrefix = normalizeWinePrefix(winePrefix);

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
        && compareField(rule.processCommand, actualProcessCommand)
        && compareField(rule.winePrefix, actualWinePrefix)
        && hasIdentity;
}

bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const QString &desktopFile,
                           const QString &resourceClass,
                           const QString &resourceName,
                           const QString &processCommand,
                           const QString &winePrefix)
{
    for (const ExcludedApplication &rule : rules) {
        if (matchesExcludedApplication(rule, desktopFile, resourceClass, resourceName,
                                       processCommand, winePrefix)) {
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
            && rule.resourceName == candidate.resourceName
            && rule.processCommand == candidate.processCommand
            && rule.winePrefix == candidate.winePrefix) {
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
            .processCommand = normalizeProcessCommand(
                object.value(QStringLiteral("processCommand")).toString()),
            .winePrefix = normalizeWinePrefix(
                object.value(QStringLiteral("winePrefix")).toString()),
            .displayName = object.value(QStringLiteral("displayName")).toString().trimmed(),
        };
        if ((application.desktopFile.isEmpty() && application.resourceClass.isEmpty()
             && application.resourceName.isEmpty() && application.processCommand.isEmpty()
             && application.winePrefix.isEmpty())
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
            .processCommand = normalizeProcessCommand(raw.processCommand),
            .winePrefix = normalizeWinePrefix(raw.winePrefix),
            .displayName = raw.displayName.trimmed(),
        };
        if ((rule.desktopFile.isEmpty() && rule.resourceClass.isEmpty()
             && rule.resourceName.isEmpty() && rule.processCommand.isEmpty()
             && rule.winePrefix.isEmpty())
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
        if (!rule.processCommand.isEmpty()) {
            object.insert(QStringLiteral("processCommand"), rule.processCommand);
        }
        if (!rule.winePrefix.isEmpty()) {
            object.insert(QStringLiteral("winePrefix"), rule.winePrefix);
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
