// SPDX-License-Identifier: GPL-3.0-or-later

#include "applicationfilter.h"

#include <QDir>
#include <QFile>
#include <QHash>
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

QString normalizedEnvironmentValue(const QHash<QByteArray, QByteArray> &environment,
                                   const QByteArray &name)
{
    return QString::fromLocal8Bit(environment.value(name)).trimmed();
}

bool isUsefulLauncherValue(const QString &value)
{
    const QString normalized = value.trimmed().toCaseFolded();
    return !normalized.isEmpty() && normalized != QLatin1String("default")
        && normalized != QLatin1String("0") && normalized != QLatin1String("umu-default");
}

QString kindToString(ApplicationIdentityKind kind)
{
    switch (kind) {
    case ApplicationIdentityKind::DesktopFile:
        return QStringLiteral("desktop-file");
    case ApplicationIdentityKind::Launcher:
        return QStringLiteral("launcher");
    case ApplicationIdentityKind::Process:
        return QStringLiteral("process");
    case ApplicationIdentityKind::WindowClass:
        return QStringLiteral("window-class");
    case ApplicationIdentityKind::Invalid:
        return {};
    }
    return {};
}

ApplicationIdentityKind kindFromString(const QString &value)
{
    if (value == QLatin1String("desktop-file")) {
        return ApplicationIdentityKind::DesktopFile;
    }
    if (value == QLatin1String("launcher")) {
        return ApplicationIdentityKind::Launcher;
    }
    if (value == QLatin1String("process")) {
        return ApplicationIdentityKind::Process;
    }
    if (value == QLatin1String("window-class")) {
        return ApplicationIdentityKind::WindowClass;
    }
    return ApplicationIdentityKind::Invalid;
}

ApplicationIdentity normalizeIdentity(ApplicationIdentity identity)
{
    switch (identity.kind) {
    case ApplicationIdentityKind::DesktopFile:
        identity.value = normalizeApplicationId(identity.value);
        identity.qualifier.clear();
        break;
    case ApplicationIdentityKind::Launcher:
        identity.value = identity.value.trimmed().toCaseFolded();
        identity.qualifier.clear();
        break;
    case ApplicationIdentityKind::Process:
        identity.value = normalizeProcessCommand(identity.value);
        identity.qualifier = normalizeWinePrefix(identity.qualifier);
        break;
    case ApplicationIdentityKind::WindowClass:
        identity.value = normalizeApplicationId(identity.value);
        identity.qualifier = normalizeApplicationId(identity.qualifier);
        break;
    case ApplicationIdentityKind::Invalid:
        identity.value.clear();
        identity.qualifier.clear();
        break;
    }
    if (identity.value.isEmpty()) {
        identity.kind = ApplicationIdentityKind::Invalid;
        identity.qualifier.clear();
    }
    return identity;
}

} // namespace

bool ApplicationIdentity::isValid() const
{
    return kind != ApplicationIdentityKind::Invalid && !value.isEmpty();
}

QString normalizeApplicationId(const QString &value)
{
    QString normalized = value.trimmed().toCaseFolded();
    if (normalized.endsWith(QLatin1String(".desktop"))) {
        normalized.chop(8);
    }
    return normalized;
}

ProcessIdentity processIdentityFromData(const QByteArray &commandLine,
                                        const QByteArray &environmentData)
{
    ProcessIdentity identity;
    identity.command = normalizeProcessCommand(
        QString::fromLocal8Bit(commandLine.split('\0').value(0)));

    QHash<QByteArray, QByteArray> environment;
    const QList<QByteArray> entries = environmentData.split('\0');
    for (const QByteArray &entry : entries) {
        const qsizetype separator = entry.indexOf('=');
        if (separator > 0) {
            environment.insert(entry.first(separator), entry.sliced(separator + 1));
        }
    }

    const QString lutrisUuid = normalizedEnvironmentValue(
        environment, QByteArrayLiteral("LUTRIS_GAME_UUID"));
    if (isUsefulLauncherValue(lutrisUuid)) {
        identity.launcherId = QStringLiteral("lutris:%1").arg(lutrisUuid.toCaseFolded());
    } else {
        const QString steamAppId = normalizedEnvironmentValue(
            environment, QByteArrayLiteral("SteamAppId"));
        const QString steamGameId = normalizedEnvironmentValue(
            environment, QByteArrayLiteral("SteamGameId"));
        const QString steamId = isUsefulLauncherValue(steamAppId) ? steamAppId : steamGameId;
        if (isUsefulLauncherValue(steamId)) {
            identity.launcherId = QStringLiteral("steam:%1").arg(steamId.toCaseFolded());
        }
    }

    identity.winePrefix = normalizeWinePrefix(normalizedEnvironmentValue(
        environment, QByteArrayLiteral("WINEPREFIX")));
    return identity;
}

ProcessIdentity processIdentity(qint64 pid)
{
    if (pid <= 0) {
        return {};
    }

    QFile commandLine(QStringLiteral("/proc/%1/cmdline").arg(pid));
    QFile environment(QStringLiteral("/proc/%1/environ").arg(pid));
    const QByteArray commandLineData = commandLine.open(QIODevice::ReadOnly)
        ? commandLine.readAll() : QByteArray();
    const QByteArray environmentData = environment.open(QIODevice::ReadOnly)
        ? environment.readAll() : QByteArray();
    return processIdentityFromData(commandLineData, environmentData);
}

ApplicationIdentity identifyApplication(const QString &desktopFile,
                                        const QString &resourceClass,
                                        const QString &resourceName,
                                        const ProcessIdentity &process)
{
    const QString normalizedDesktopFile = normalizeApplicationId(desktopFile);
    if (!normalizedDesktopFile.isEmpty()) {
        return {ApplicationIdentityKind::DesktopFile, normalizedDesktopFile, {}};
    }
    if (!process.launcherId.isEmpty()) {
        return normalizeIdentity({ApplicationIdentityKind::Launcher,
                                  process.launcherId, {}});
    }
    if (!process.command.isEmpty()) {
        return normalizeIdentity({ApplicationIdentityKind::Process,
                                  process.command, process.winePrefix});
    }

    const QString normalizedClass = normalizeApplicationId(resourceClass);
    const QString normalizedName = normalizeApplicationId(resourceName);
    if (!normalizedClass.isEmpty()) {
        return {ApplicationIdentityKind::WindowClass, normalizedClass, normalizedName};
    }
    if (!normalizedName.isEmpty()) {
        return {ApplicationIdentityKind::WindowClass, normalizedName, {}};
    }
    return {};
}

bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const ApplicationIdentity &candidate)
{
    if (!candidate.isValid()) {
        return false;
    }
    for (const ExcludedApplication &rule : rules) {
        if (rule.identity == candidate) {
            return true;
        }
    }
    return false;
}

bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate)
{
    return isApplicationExcluded(rules, candidate.identity);
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
            .identity = normalizeIdentity({
                kindFromString(object.value(QStringLiteral("kind")).toString()),
                object.value(QStringLiteral("value")).toString(),
                object.value(QStringLiteral("qualifier")).toString(),
            }),
            .displayName = object.value(QStringLiteral("displayName")).toString().trimmed(),
        };
        if (!application.identity.isValid()
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
            .identity = normalizeIdentity(raw.identity),
            .displayName = raw.displayName.trimmed(),
        };
        if (!rule.identity.isValid() || containsExcludedApplication(unique, rule)) {
            continue;
        }
        unique.push_back(rule);

        QJsonObject object{
            {QStringLiteral("kind"), kindToString(rule.identity.kind)},
            {QStringLiteral("value"), rule.identity.value},
        };
        if (!rule.identity.qualifier.isEmpty()) {
            object.insert(QStringLiteral("qualifier"), rule.identity.qualifier);
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
