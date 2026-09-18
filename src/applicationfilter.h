// SPDX-License-Identifier: GPL-3.0-or-later
// 应用排除规则的身份选择、序列化与匹配逻辑。

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <QVector>

namespace baclickfx
{

enum class ApplicationIdentityKind {
    Invalid,
    DesktopFile,
    Launcher,
    Process,
    WindowClass,
};

struct ApplicationIdentity {
    ApplicationIdentityKind kind = ApplicationIdentityKind::Invalid;
    QString value;
    QString qualifier;

    bool isValid() const;
    bool operator==(const ApplicationIdentity &) const = default;
};

struct ProcessIdentity {
    QString launcherId;
    QString command;
    QString winePrefix;
};

struct ExcludedApplication {
    ApplicationIdentity identity;
    QString displayName;
};

/// 将 desktop-file ID 或窗口类规范化，避免大小写、空白和后缀影响比较。
QString normalizeApplicationId(const QString &value);

/// 解析 /proc 的 NUL 分隔内容；独立接口便于覆盖启动器优先级的单元测试。
ProcessIdentity processIdentityFromData(const QByteArray &commandLine,
                                        const QByteArray &environment);

/// 从进程信息中提取可跨重启复用的身份；PID 本身不会写入规则。
ProcessIdentity processIdentity(qint64 pid);

/**
 * 选择单一权威身份，优先级为 desktop file、启动器 ID、进程、窗口类。
 *
 * 规则只保存返回的这一层身份，低优先级字段不会参与后续匹配。
 */
ApplicationIdentity identifyApplication(const QString &desktopFile,
                                        const QString &resourceClass,
                                        const QString &resourceName,
                                        const ProcessIdentity &process = {});

bool isApplicationExcluded(const QVector<ExcludedApplication> &rules,
                           const ApplicationIdentity &candidate);
bool containsExcludedApplication(const QVector<ExcludedApplication> &rules,
                                 const ExcludedApplication &candidate);

QVector<ExcludedApplication> excludedApplicationsFromJson(const QJsonArray &array);
QJsonArray excludedApplicationsToJson(const QVector<ExcludedApplication> &rules);
QVector<ExcludedApplication> parseExcludedApplications(const QByteArray &json);
QByteArray serializeExcludedApplications(const QVector<ExcludedApplication> &rules);

} // namespace baclickfx
