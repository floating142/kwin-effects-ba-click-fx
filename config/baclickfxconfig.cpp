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
    connect(m_ui.enableDistanceEmitterCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.debugLogCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);
    connect(m_ui.debugDamageCheckBox, &QCheckBox::toggled,
            this, &BaClickFxEffectConfig::markAsChanged);

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
    m_ui.enableDistanceEmitterCheckBox->setChecked(
        conf.readEntry(def::kEnableDistanceEmitter, def::kEnableDistanceEmitterDefault));
    m_ui.debugLogCheckBox->setChecked(conf.readEntry(def::kDebugLog, def::kDebugLogDefault));
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
    conf.writeEntry(def::kEnableDistanceEmitter,
                    m_ui.enableDistanceEmitterCheckBox->isChecked());
    conf.writeEntry(def::kDebugLog, m_ui.debugLogCheckBox->isChecked());
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
    m_ui.enableDistanceEmitterCheckBox->setChecked(def::kEnableDistanceEmitterDefault);
    m_ui.debugLogCheckBox->setChecked(def::kDebugLogDefault);
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
