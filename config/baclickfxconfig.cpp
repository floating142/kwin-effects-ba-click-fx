// SPDX-License-Identifier: GPL-3.0-or-later
// BA Click FX KCM 实现。配置键、默认值和范围与特效本体共用 baclickfxdefaults.h。

#include "baclickfxconfig.h"

#include "baclickfxdefaults.h"
#include "outputscaleutils.h"

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
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QProcess>
#include <QTimer>
#include <QStandardPaths>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScreen>
#include <QSlider>
#include <QPushButton>
#include <QToolButton>
#include <QIcon>
#include <QLabel>
#include <QHBoxLayout>

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
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("kwin_ba_click_fx_config"));
    // setupUi() 负责为 KCModule 页面安装布局。
    m_ui.setupUi(widget());

    const auto showDebugOptions = [this](bool visible) {
        m_ui.logLevelLabel->setVisible(visible);
        m_ui.logLevelComboBox->setVisible(visible);
        m_ui.debugDamageCheckBox->setVisible(visible);
        m_ui.copyDiagnosticsButton->setVisible(visible);
        m_ui.generateDiagnosticsButton->setVisible(visible);
        m_ui.openDiagnosticsDirButton->setVisible(visible);
    };
    connect(m_ui.showDebugOptionsCheckBox, &QCheckBox::toggled,
            this, showDebugOptions);
    showDebugOptions(false);

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
                m_ui.copyDiagnosticsButton->setText(i18n("Copied"));
                QTimer::singleShot(1500, this, [this]() {
                    m_ui.copyDiagnosticsButton->setText(i18n("Copy diagnostics"));
                });
            } else {
                m_ui.copyDiagnosticsButton->setText(i18n("Failed"));
                QTimer::singleShot(1500, this, [this]() {
                    m_ui.copyDiagnosticsButton->setText(i18n("Copy diagnostics"));
                });
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
                    report.close();
                    auto *journal = new QProcess(this);
                    const QString reportPath = path;
                    connect(journal, &QProcess::finished, this,
                            [this, journal, reportPath](int, QProcess::ExitStatus) {
                        QFile reportFile(reportPath);
                        if (reportFile.open(QIODevice::Append | QIODevice::Text)) {
                            reportFile.write(journal->readAllStandardOutput());
                            reportFile.close();
                            m_ui.generateDiagnosticsButton->setText(i18n("Generated"));
                        } else {
                            m_ui.generateDiagnosticsButton->setText(i18n("Failed"));
                        }
                        QTimer::singleShot(1500, this, [this]() {
                            m_ui.generateDiagnosticsButton->setText(i18n("Generate report"));
                        });
                        journal->deleteLater();
                    });
                    journal->start(QStringLiteral("journalctl"),
                                   {QStringLiteral("--user"), QStringLiteral("--since=-2min"),
                                    QStringLiteral("-n"), QStringLiteral("200"),
                                    QStringLiteral("-o"), QStringLiteral("cat"),
                                    QStringLiteral("QT_CATEGORY=kwin_effect_ba_click_fx")});
                } else {
                    m_ui.generateDiagnosticsButton->setText(i18n("Failed"));
                    QTimer::singleShot(1500, this, [this]() {
                        m_ui.generateDiagnosticsButton->setText(i18n("Generate report"));
                    });
                }
            } else {
                m_ui.generateDiagnosticsButton->setText(i18n("Failed"));
                QTimer::singleShot(1500, this, [this]() {
                    m_ui.generateDiagnosticsButton->setText(i18n("Generate report"));
                });
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
    connect(m_ui.showOutputScaleCheckBox, &QCheckBox::toggled, this, [this](bool enabled) {
        m_ui.outputScaleGroupBox->setVisible(enabled && !m_outputSliders.isEmpty());
        markAsChanged();
    });
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *) {
        if (!needsSave()) m_outputRefreshDebounce.start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
        if (!needsSave()) m_outputRefreshDebounce.start();
    });
    m_outputRefreshDebounce.setSingleShot(true);
    m_outputRefreshDebounce.setInterval(100);
    connect(&m_outputRefreshDebounce, &QTimer::timeout, this, [this]() {
        if (!needsSave()) refreshOutputMetadata();
    });
    m_outputRefreshTimeout.setSingleShot(true);
    m_outputRefreshTimeout.setInterval(2000);
    connect(&m_outputRefreshTimeout, &QTimer::timeout, this, [this]() {
        if (m_outputRefreshWatcher) {
            m_outputRefreshWatcher->disconnect(this);
            m_outputRefreshWatcher->deleteLater();
            m_outputRefreshWatcher = nullptr;
        }
    });
}

void BaClickFxEffectConfig::rebuildOutputScaleEditors()
{
    auto *layout = m_ui.outputScaleGroupBox->findChild<QVBoxLayout *>(QStringLiteral("outputScaleLayout"));
    if (!layout) return;
    layout->setContentsMargins(12, 8, 12, 10);
    layout->setSpacing(8);
    if (m_outputScaleContainer) {
        layout->removeWidget(m_outputScaleContainer);
        m_outputScaleContainer->deleteLater();
        m_outputScaleContainer = nullptr;
    }
    m_outputSliders.clear();
    m_outputLabels.clear();
    m_outputNames.clear();
    m_outputResetButtons.clear();
    m_outputOverrides.clear();
    auto *container = new QWidget(m_ui.outputScaleGroupBox);
    auto *rows = new QVBoxLayout(container);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(8);
    m_outputScaleContainer = container;
    const QJsonObject values = QJsonDocument::fromJson(
        KSharedConfig::openConfig(QStringLiteral("kwinrc"))
            ->group(QLatin1String(def::kGroup))
            .readEntry(def::kOutputScaleOverrides, QByteArray())).object();
    const double global = m_ui.globalScaleSlider->value() / double(kSliderScale);
    for (QScreen *screen : QGuiApplication::screens()) {
        const QString fallbackId = baclickfx::outputScaleId(
            screen->manufacturer(), screen->model(), screen->serialNumber(), screen->name());
        const QString id = m_outputIds.value(screen->name(), fallbackId);
        auto *rowWidget = new QWidget(container);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        // QScreen::devicePixelRatio() may be rounded independently of KWin's
        // fractional output scale. Logical DPI preserves the actual 1.60 factor.
        const QSize native = m_outputNativeSizes.value(screen->name(), screen->size());
        const QString displayName = QStringLiteral("%1 (%2x%3)")
            .arg(screen->name())
            .arg(native.width())
            .arg(native.height());
        auto *name = new QLabel(displayName, rowWidget);
        name->setMinimumWidth(180);
        auto *slider = new QSlider(Qt::Horizontal, rowWidget);
        auto *value = new QLabel(rowWidget);
        auto *reset = new QToolButton(rowWidget);
        reset->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo")));
        reset->setAccessibleName(i18n("Reset to 1.00"));
        slider->setRange(int(def::kGlobalScaleMin * kSliderScale), int(def::kGlobalScaleMax * kSliderScale));
        const bool has = values.contains(id);
        if (has) m_outputOverrides.insert(id);
        slider->setValue(int((has ? values.value(id).toDouble(global) : global) * kSliderScale));
        value->setMinimumWidth(40);
        reset->setAutoRaise(true);
        row->addWidget(name);
        row->addWidget(slider, 1);
        row->addWidget(value);
        row->addWidget(reset);
        rows->addWidget(rowWidget);
        m_outputSliders.insert(id, slider);
        m_outputLabels.insert(id, value);
        m_outputNames.insert(id, name);
        m_outputResetButtons.insert(id, reset);
        value->setText(QString::number(slider->value() / double(kSliderScale), 'f', 2));
        reset->setEnabled(slider->value() != int(def::kGlobalScaleDefault * kSliderScale));
        connect(slider, &QSlider::valueChanged, this, [this, id](int v) {
            m_outputOverrides.insert(id);
            m_outputLabels.value(id)->setText(QString::number(v / double(kSliderScale), 'f', 2));
            m_outputResetButtons.value(id)->setEnabled(
                v != int(def::kGlobalScaleDefault * kSliderScale));
            markAsChanged();
        });
        connect(reset, &QToolButton::clicked, this, [this, id]() {
            QSlider *output = m_outputSliders.value(id);
            if (!output) return;
            QSignalBlocker blocker(output);
            output->setValue(int(def::kGlobalScaleDefault * kSliderScale));
            m_outputOverrides.insert(id);
            m_outputLabels.value(id)->setText(QString::number(
                def::kGlobalScaleDefault, 'f', 2));
            m_outputResetButtons.value(id)->setEnabled(false);
            markAsChanged();
        });
    }
    layout->addWidget(container);
    m_ui.outputScaleGroupBox->setVisible(m_ui.showOutputScaleCheckBox->isChecked()
                                         && !m_outputSliders.isEmpty());
}

void BaClickFxEffectConfig::refreshOutputMetadata()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"),
        QStringLiteral("org.kde.kwin.Effects"), QStringLiteral("debug"));
    message << QStringLiteral("kwin4_effect_ba_click_fx") << QStringLiteral("outputs-json");
    if (m_outputRefreshWatcher) {
        m_outputRefreshWatcher->deleteLater();
        m_outputRefreshWatcher = nullptr;
    }
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(message), this);
    m_outputRefreshWatcher = watcher;
    m_outputRefreshTimeout.start();
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *call) {
        const QDBusPendingReply<QString> reply = *call;
        if (!reply.isError()) {
            const QJsonDocument document = QJsonDocument::fromJson(reply.value().toUtf8());
            if (document.isArray()) {
                m_outputIds.clear();
                m_outputNativeSizes.clear();
                for (const QJsonValue &value : document.array()) {
                    const QJsonObject output = value.toObject();
                    const QString name = output.value(QStringLiteral("name")).toString();
                    const QString id = output.value(QStringLiteral("uuid")).toString().isEmpty()
                        ? output.value(QStringLiteral("id")).toString()
                        : output.value(QStringLiteral("uuid")).toString();
                    if (name.isEmpty() || id.isEmpty()) continue;
                    m_outputIds.insert(name, id);
                    m_outputNativeSizes.insert(name, QSize(
                        output.value(QStringLiteral("pixelWidth")).toInt(),
                        output.value(QStringLiteral("pixelHeight")).toInt()));
                }
                if (!needsSave()) rebuildOutputScaleEditors();
            }
        }
        call->deleteLater();
        m_outputRefreshTimeout.stop();
        if (m_outputRefreshWatcher == call) m_outputRefreshWatcher = nullptr;
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
    {
        QSignalBlocker blocker(m_ui.showOutputScaleCheckBox);
        m_ui.showOutputScaleCheckBox->setChecked(
            conf.readEntry(def::kOutputScaleEnabled, def::kOutputScaleEnabledDefault));
    }
    rebuildOutputScaleEditors();
    refreshOutputMetadata();

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
    setNeedsSave(false);
}

void BaClickFxEffectConfig::save()
{
    KConfigGroup conf = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                            ->group(QLatin1String(def::kGroup));

    conf.writeEntry(def::kTimeScale, m_ui.timeScaleSlider->value() / double(kSliderScale));
    conf.writeEntry(def::kGlobalScale, m_ui.globalScaleSlider->value() / double(kSliderScale));
    QJsonObject overrides = QJsonDocument::fromJson(
        conf.readEntry(def::kOutputScaleOverrides, QByteArray())).object();
    for (auto it = m_outputSliders.cbegin(); it != m_outputSliders.cend(); ++it) {
        if (m_outputOverrides.contains(it.key())) {
            overrides.insert(it.key(), it.value()->value() / double(kSliderScale));
        } else {
            overrides.remove(it.key());
        }
    }
    conf.writeEntry(def::kOutputScaleOverrides,
                    QJsonDocument(overrides).toJson(QJsonDocument::Compact));
    conf.writeEntry(def::kOutputScaleEnabled, m_ui.showOutputScaleCheckBox->isChecked());

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
    m_ui.showOutputScaleCheckBox->setChecked(def::kOutputScaleEnabledDefault);
    m_outputOverrides.clear();
    for (auto it = m_outputSliders.cbegin(); it != m_outputSliders.cend(); ++it) {
        QSignalBlocker blocker(it.value());
        it.value()->setValue(int(def::kGlobalScaleDefault * kSliderScale));
        if (QLabel *label = m_outputLabels.value(it.key())) {
            label->setText(QStringLiteral("1.00"));
        }
        if (QToolButton *reset = m_outputResetButtons.value(it.key())) {
            reset->setEnabled(false);
        }
    }
    // Defaults must not destroy and recreate child widgets while KCModule is
    // processing its reset action. The persisted overrides are cleared on save.

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
