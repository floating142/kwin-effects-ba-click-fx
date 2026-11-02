// SPDX-License-Identifier: GPL-3.0-or-later
#include <KCModule>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KSharedConfig>

#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class EffectsStub final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Effects")
public:
    QJsonObject lastPreview;

public Q_SLOTS:
    QString debug(const QString &, const QString &parameter)
    {
        if (parameter.startsWith(QLatin1String("preview:"))) {
            lastPreview = QJsonDocument::fromJson(parameter.mid(8).toUtf8()).object();
        }
        return parameter == QLatin1String("outputs-json") ? QStringLiteral("[]") : QString();
    }
    void reconfigureEffect(const QString &) {}
};

class ConfigTests final : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void triggerButtonsSavePreviewCancelAndDefaults();

private:
    EffectsStub m_effects;
};

void ConfigTests::initTestCase()
{
    // 在加载模块前取得服务名；若误连宿主会话，此处失败，绝不发送预览。
    QVERIFY(QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.KWin")));
    QVERIFY(QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/Effects"), &m_effects, QDBusConnection::ExportAllSlots));
}

void ConfigTests::triggerButtonsSavePreviewCancelAndDefaults()
{
    const KPluginMetaData metadata(QStringLiteral(BA_CLICK_FX_CONFIG_PLUGIN_PATH));
    const auto createModule = [&]() {
        return std::unique_ptr<KCModule>(KPluginFactory::instantiatePlugin<KCModule>(metadata).plugin);
    };
    const auto buttons = [](KCModule *module) {
        int mask = 0;
        const auto *root = module->widget();
        if (root->findChild<QCheckBox *>(QStringLiteral("leftButtonCheckBox"))->isChecked()) mask |= 1;
        if (root->findChild<QCheckBox *>(QStringLiteral("rightButtonCheckBox"))->isChecked()) mask |= 2;
        if (root->findChild<QCheckBox *>(QStringLiteral("middleButtonCheckBox"))->isChecked()) mask |= 4;
        return mask;
    };
    auto module = createModule();
    QVERIFY(module);
    module->load();
    for (const auto *name : {"leftButtonCheckBox", "rightButtonCheckBox", "middleButtonCheckBox"}) {
        QVERIFY(module->widget()->findChild<QCheckBox *>(QLatin1String(name)));
    }
    QCOMPARE(buttons(module.get()), 1); // 没有新配置的用户仍仅由左键触发。
    auto *left = module->widget()->findChild<QCheckBox *>(QStringLiteral("leftButtonCheckBox"));
    auto *right = module->widget()->findChild<QCheckBox *>(QStringLiteral("rightButtonCheckBox"));
    auto *middle = module->widget()->findChild<QCheckBox *>(QStringLiteral("middleButtonCheckBox"));
    left->setChecked(false);
    right->setChecked(true);
    middle->setChecked(true);
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("triggerButtons")).toInt(-1), 6);
    module->save();
    const auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    const KConfigGroup group(config, QStringLiteral("Effect-ba-click-fx"));
    QCOMPARE(group.readEntry("TriggerButtons", -1), 6);
    module.reset();

    module = createModule();
    QVERIFY(module);
    module->load();
    QCOMPARE(buttons(module.get()), 6);
    module->defaults();
    QCOMPARE(buttons(module.get()), 1);
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("triggerButtons")).toInt(-1), 1);
    QCOMPARE(group.readEntry("TriggerButtons", -1), 6); // 未应用默认值不得覆盖持久配置。
    module.reset(); // 取消默认值预览，应将已保存的右键 + 中键发回 KWin。
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("triggerButtons")).toInt(-1), 6);

    module = createModule();
    QVERIFY(module);
    module->load();
    QCOMPARE(buttons(module.get()), 6);
    for (const auto *name : {"leftButtonCheckBox", "rightButtonCheckBox", "middleButtonCheckBox"}) {
        module->widget()->findChild<QCheckBox *>(QLatin1String(name))->setChecked(false);
    }
    auto *alwaysTrail = module->widget()->findChild<QCheckBox *>(QStringLiteral("alwaysTrailCheckBox"));
    QVERIFY(alwaysTrail);
    QVERIFY(alwaysTrail->isEnabled());
    alwaysTrail->setChecked(true);
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("triggerButtons")).toInt(-1), 0);
    QTRY_VERIFY(m_effects.lastPreview.value(QStringLiteral("alwaysTrail")).toBool());
    module->save();
    module->load();
    QCOMPARE(buttons(module.get()), 0);
    QVERIFY(alwaysTrail->isChecked());
}

int main(int argc, char **argv)
{
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
    ConfigTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "config_tests.moc"
