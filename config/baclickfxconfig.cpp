// SPDX-License-Identifier: GPL-3.0-or-later
// BA Click FX KCM 实现。配置键、默认值和范围与特效本体共用 baclickfxdefaults.h。

#include "baclickfxconfig.h"

#include "baclickfxdefaults.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace def = baclickfx::defaults;

namespace
{
// QSlider 仅保存整数，因此小数配置统一缩放 100 倍。
constexpr int kSliderScale = 100;
}

K_PLUGIN_CLASS(BaClickFxEffectConfig)

BaClickFxEffectConfig::BaClickFxEffectConfig(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    // setupUi() 负责为 KCModule 页面安装布局。
    m_ui.setupUi(widget());

    // 运行时范围来自共享默认值；.ui 中的范围仅用于 Designer 预览。
    m_ui.timeScaleSlider->setRange(int(def::kTimeScaleMin * kSliderScale),
                                   int(def::kTimeScaleMax * kSliderScale));
    m_ui.globalScaleSlider->setRange(int(def::kGlobalScaleMin * kSliderScale),
                                     int(def::kGlobalScaleMax * kSliderScale));

    // 所有可编辑控件变化时由 KCModule 更新「应用」按钮状态。
    connect(m_ui.timeScaleSlider, &QSlider::valueChanged,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.globalScaleSlider, &QSlider::valueChanged,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.enableTrailCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.alwaysTrailCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.enableDistanceEmitterCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.enableTrailCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        m_ui.alwaysTrailCheckBox->setEnabled(enabled);
        m_ui.enableDistanceEmitterCheckBox->setEnabled(enabled);
    });
    connect(m_ui.logLevelComboBox, &QComboBox::currentIndexChanged,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.debugDamageCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.copyDiagnosticsButton, &QPushButton::clicked, this, [this]() {
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"),
            QStringLiteral("/Effects"),
            QStringLiteral("org.kde.kwin.Effects"),
            QStringLiteral("debug"));
        message << QStringLiteral("kwin4_effect_ba_click_fx")
                << QStringLiteral("diagnostics");

        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<QString> reply = *call;
            if (!reply.isError() && !reply.value().isEmpty()) {
                QApplication::clipboard()->setText(reply.value());
                m_ui.copyDiagnosticsButton->setText(i18n("已复制诊断信息"));
            } else {
                m_ui.copyDiagnosticsButton->setText(i18n("无法获取诊断信息"));
            }
            call->deleteLater();
        });
    });
    connect(m_ui.openDiagnosticsDirButton, &QPushButton::clicked, this, []() {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
            + QStringLiteral("/ba-click-fx/diagnostics");
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(m_ui.generateDiagnosticsButton, &QPushButton::clicked, this, [this]() {
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"),
            QStringLiteral("org.kde.kwin.Effects"), QStringLiteral("debug"));
        message << QStringLiteral("kwin4_effect_ba_click_fx")
                << QStringLiteral("diagnostics");
        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<QString> reply = *call;
            if (!reply.isError() && !reply.value().isEmpty()) {
                const QString dir = QStandardPaths::writableLocation(
                    QStandardPaths::GenericCacheLocation)
                    + QStringLiteral("/ba-click-fx/diagnostics");
                QDir().mkpath(dir);
                const QString path = dir + QStringLiteral("/ba-click-fx-")
                    + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))
                    + QStringLiteral(".txt");
                QFile report(path);
                if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    report.write("[diagnostics]\n");
                    report.write(reply.value().toUtf8());
                    report.write("\n\n[recent_logs]\n");
                    QProcess journal;
                    journal.start(QStringLiteral("journalctl"),
                                  {QStringLiteral("--user"), QStringLiteral("--since=-2min"),
                                   QStringLiteral("-n"), QStringLiteral("200"),
                                   QStringLiteral("-o"), QStringLiteral("cat"),
                                   QStringLiteral("QT_CATEGORY=kwin_effect_ba_click_fx")});
                    if (journal.waitForFinished(3000)) {
                        report.write(journal.readAllStandardOutput());
                    }
                    m_ui.generateDiagnosticsButton->setText(i18n("诊断报告已生成"));
                }
            } else {
                m_ui.generateDiagnosticsButton->setText(i18n("无法生成诊断报告"));
            }
            call->deleteLater();
        });
    });

    // 滑块变化时同步刷新数值标签。
    connect(m_ui.timeScaleSlider, &QSlider::valueChanged, this, [this](int value) {
        m_ui.timeScaleValueLabel->setText(QString::number(value / double(kSliderScale), 'f', 2));
    });
    connect(m_ui.globalScaleSlider, &QSlider::valueChanged, this, [this](int value) {
        m_ui.globalScaleValueLabel->setText(QString::number(value / double(kSliderScale), 'f', 2));
    });
}

void BaClickFxEffectConfig::load()
{
    KCModule::load();

    const KConfigGroup conf = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                                  ->group(QLatin1String(def::kGroup));

    m_ui.timeScaleSlider->setValue(
        int(conf.readEntry(def::kTimeScale, def::kTimeScaleDefault) * kSliderScale));
    m_ui.globalScaleSlider->setValue(
        int(conf.readEntry(def::kGlobalScale, def::kGlobalScaleDefault) * kSliderScale));

    m_ui.enableTrailCheckBox->setChecked(
        conf.readEntry(def::kEnableTrail, def::kEnableTrailDefault));
    m_ui.alwaysTrailCheckBox->setChecked(
        conf.readEntry(def::kAlwaysTrail, def::kAlwaysTrailDefault));
    m_ui.enableDistanceEmitterCheckBox->setChecked(
        conf.readEntry(def::kEnableDistanceEmitter, def::kEnableDistanceEmitterDefault));
    m_ui.alwaysTrailCheckBox->setEnabled(m_ui.enableTrailCheckBox->isChecked());
    m_ui.enableDistanceEmitterCheckBox->setEnabled(m_ui.enableTrailCheckBox->isChecked());
    m_ui.logLevelComboBox->setCurrentIndex(std::clamp(
        conf.readEntry(def::kLogLevel, int(def::kLogLevelDefault)),
        int(def::LogLevel::Off), int(def::LogLevel::Verbose)));
    m_ui.debugDamageCheckBox->setChecked(
        conf.readEntry(def::kDebugDamage, def::kDebugDamageDefault));

    // setValue() 在数值未变化时不会发出信号，因此加载后显式刷新标签。
    updateValueLabels();
}

void BaClickFxEffectConfig::save()
{
    KConfigGroup conf = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                            ->group(QLatin1String(def::kGroup));

    conf.writeEntry(def::kTimeScale, m_ui.timeScaleSlider->value() / double(kSliderScale));
    conf.writeEntry(def::kGlobalScale, m_ui.globalScaleSlider->value() / double(kSliderScale));

    conf.writeEntry(def::kEnableTrail, m_ui.enableTrailCheckBox->isChecked());
    conf.writeEntry(def::kAlwaysTrail, m_ui.alwaysTrailCheckBox->isChecked());
    conf.writeEntry(def::kEnableDistanceEmitter,
                    m_ui.enableDistanceEmitterCheckBox->isChecked());
    conf.writeEntry(def::kLogLevel, m_ui.logLevelComboBox->currentIndex());
    conf.writeEntry(def::kDebugDamage, m_ui.debugDamageCheckBox->isChecked());

    conf.sync();

    KCModule::save();

    // 使用进程内 D-Bus 通知 KWin 重新读取配置，避免依赖发行版特定的 qdbus 名称。
    // KWin 或特效未运行时异步调用可安全失败。
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/Effects"),
        QStringLiteral("org.kde.kwin.Effects"),
        QStringLiteral("reconfigureEffect"));
    message << QStringLiteral("kwin4_effect_ba_click_fx");
    QDBusConnection::sessionBus().asyncCall(message);
}

void BaClickFxEffectConfig::defaults()
{
    m_ui.timeScaleSlider->setValue(int(def::kTimeScaleDefault * kSliderScale));
    m_ui.globalScaleSlider->setValue(int(def::kGlobalScaleDefault * kSliderScale));

    m_ui.enableTrailCheckBox->setChecked(def::kEnableTrailDefault);
    m_ui.alwaysTrailCheckBox->setChecked(def::kAlwaysTrailDefault);
    m_ui.enableDistanceEmitterCheckBox->setChecked(def::kEnableDistanceEmitterDefault);
    m_ui.logLevelComboBox->setCurrentIndex(int(def::kLogLevelDefault));
    m_ui.debugDamageCheckBox->setChecked(def::kDebugDamageDefault);

    updateValueLabels();
    markAsChanged();

    KCModule::defaults();
}

void BaClickFxEffectConfig::updateValueLabels()
{
    m_ui.timeScaleValueLabel->setText(
        QString::number(m_ui.timeScaleSlider->value() / double(kSliderScale), 'f', 2));
    m_ui.globalScaleValueLabel->setText(
        QString::number(m_ui.globalScaleSlider->value() / double(kSliderScale), 'f', 2));
}

#include "baclickfxconfig.moc"
