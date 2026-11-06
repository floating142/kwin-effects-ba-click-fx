// SPDX-License-Identifier: GPL-3.0-or-later
#include <KCModule>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KSharedConfig>

#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>

class EffectsStub final : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Effects")
public:
    QJsonObject lastPreview;
    bool delayOutputReply = false;
    int outputRequests = 0;
    QDBusMessage outputRequest;

    void replyWithOutputs(const QJsonArray &outputs)
    {
        const auto payload = QString::fromUtf8(QJsonDocument(outputs).toJson(QJsonDocument::Compact));
        QDBusConnection(QStringLiteral("ba-click-fx-test-service")).send(outputRequest.createReply(payload));
        outputRequest = {};
        delayOutputReply = false;
    }

public Q_SLOTS:
    QString debug(const QString &, const QString &parameter)
    {
        if (parameter.startsWith(QLatin1String("preview:"))) {
            lastPreview = QJsonDocument::fromJson(parameter.mid(8).toUtf8()).object();
        }
        if (parameter == QLatin1String("outputs-json")) {
            ++outputRequests;
            if (delayOutputReply) {
                setDelayedReply(true);
                outputRequest = message();
            }
            return QStringLiteral("[]");
        }
        return {};
    }
    void reconfigureEffect(const QString &) {}
};

class ConfigTests final : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void triggerButtonsSavePreviewCancelAndDefaults();
    void newDisplayKeepsDefaultScaleWhenSavingOtherSettings();
    void delayedOutputMetadataUsesUuidWhenEditingScale();
    void screenRefreshWaitsForPendingEdits();

private:
    EffectsStub m_effects;
};

void ConfigTests::initTestCase()
{
    // 在加载模块前取得服务名；若误连宿主会话，此处失败，绝不发送预览。
    // 使用独立连接，让测试桩能延后回复异步请求。
    auto service = QDBusConnection::connectToBus(
        QDBusConnection::SessionBus, QStringLiteral("ba-click-fx-test-service"));
    QVERIFY(service.registerService(QStringLiteral("org.kde.KWin")));
    QVERIFY(service.registerObject(
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

void ConfigTests::newDisplayKeepsDefaultScaleWhenSavingOtherSettings()
{
    const auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup group(config, QStringLiteral("Effect-ba-click-fx"));
    group.writeEntry("GlobalScale", 1.5);
    group.writeEntry("OutputScaleEnabled", true);
    const QJsonObject savedOverrides{{QStringLiteral("disconnected-display"), 1.234}};
    group.writeEntry("OutputScaleOverrides", QJsonDocument(savedOverrides).toJson(QJsonDocument::Compact));
    group.sync();

    const KPluginMetaData metadata(QStringLiteral(BA_CLICK_FX_CONFIG_PLUGIN_PATH));
    auto module = std::unique_ptr<KCModule>(KPluginFactory::instantiatePlugin<KCModule>(metadata).plugin);
    QVERIFY(module);
    module->load();
    auto *editor = module->widget()->findChild<QGroupBox *>(QStringLiteral("outputScaleGroupBox"));
    QVERIFY(editor);
    auto *slider = editor->findChild<QSlider *>();
    QVERIFY(slider);
    QCOMPARE(slider->value(), 100);

    auto *click = module->widget()->findChild<QCheckBox *>(QStringLiteral("enableClickCheckBox"));
    QVERIFY(click);
    click->toggle();
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("outputScaleOverrides")).toObject(), savedOverrides);
    module->save();
    QCOMPARE(QJsonDocument::fromJson(group.readEntry("OutputScaleOverrides", QByteArray())).object(),
             savedOverrides);
}

void ConfigTests::delayedOutputMetadataUsesUuidWhenEditingScale()
{
    const auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup group(config, QStringLiteral("Effect-ba-click-fx"));
    const QString uuid = QStringLiteral("display-uuid");
    QCOMPARE(QGuiApplication::primaryScreen()->name(), QStringLiteral("Test-1"));
    const QJsonObject savedOverrides{{uuid, 1.4}, {QStringLiteral("disconnected-display"), 1.234}};
    group.writeEntry("OutputScaleEnabled", true);
    group.writeEntry("OutputScaleOverrides", QJsonDocument(savedOverrides).toJson(QJsonDocument::Compact));
    group.sync();

    m_effects.delayOutputReply = true;
    const KPluginMetaData metadata(QStringLiteral(BA_CLICK_FX_CONFIG_PLUGIN_PATH));
    auto module = std::unique_ptr<KCModule>(KPluginFactory::instantiatePlugin<KCModule>(metadata).plugin);
    QVERIFY(module);
    module->load();
    auto *editor = module->widget()->findChild<QGroupBox *>(QStringLiteral("outputScaleGroupBox"));
    QVERIFY(editor);
    const auto slider = [editor]() { return editor->findChild<QSlider *>(); };
    QVERIFY(slider());
    QVERIFY(!slider()->isEnabled());

    // 即使其他选项已经编辑，收到 UUID 后也应加载正确的逐屏值。
    module->widget()->findChild<QCheckBox *>(QStringLiteral("enableClickCheckBox"))->toggle();
    QTRY_COMPARE(m_effects.outputRequest.type(), QDBusMessage::MethodCallMessage);
    m_effects.replyWithOutputs({QJsonObject{{QStringLiteral("name"), QGuiApplication::primaryScreen()->name()},
                                           {QStringLiteral("uuid"), uuid},
                                           {QStringLiteral("pixelWidth"), 128},
                                           {QStringLiteral("pixelHeight"), 96}}});
    QTRY_VERIFY(slider() && slider()->isEnabled());
    QCOMPARE(slider()->value(), 140);
    slider()->setValue(165);
    QJsonObject expected = savedOverrides;
    expected.insert(uuid, 1.65);
    QTRY_COMPARE(m_effects.lastPreview.value(QStringLiteral("outputScaleOverrides")).toObject(), expected);
    module->save();
    QCOMPARE(QJsonDocument::fromJson(group.readEntry("OutputScaleOverrides", QByteArray())).object(), expected);
}

void ConfigTests::screenRefreshWaitsForPendingEdits()
{
    const int beforeLoad = m_effects.outputRequests;
    const KPluginMetaData metadata(QStringLiteral(BA_CLICK_FX_CONFIG_PLUGIN_PATH));
    auto module = std::unique_ptr<KCModule>(KPluginFactory::instantiatePlugin<KCModule>(metadata).plugin);
    QVERIFY(module);
    module->load();
    auto *editor = module->widget()->findChild<QGroupBox *>(QStringLiteral("outputScaleGroupBox"));
    QVERIFY(editor);
    const auto slider = [editor]() { return editor->findChild<QSlider *>(); };
    QTRY_COMPARE(m_effects.outputRequests, beforeLoad + 1);
    QTRY_VERIFY(slider() && slider()->isEnabled());
    slider()->setValue(125);
    QVERIFY(module->needsSave());

    Q_EMIT qApp->screenAdded(QGuiApplication::primaryScreen());
    Q_EMIT qApp->screenRemoved(QGuiApplication::primaryScreen());
    QTest::qWait(150);
    QCOMPARE(m_effects.outputRequests, beforeLoad + 1);
    QCOMPARE(slider()->value(), 125);

    module->save();
    QTRY_COMPARE(m_effects.outputRequests, beforeLoad + 2);
    QTRY_VERIFY(slider() && slider()->isEnabled());
    QCOMPARE(slider()->value(), 125);
}

int main(int argc, char **argv)
{
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    const auto displayConfig = config.filePath(QStringLiteral("offscreen.json"));
    QFile displays(displayConfig);
    if (!displays.open(QIODevice::WriteOnly)
        || displays.write(R"({"screens":[{"name":"Test-1","width":1024,"height":768}]})") < 0) {
        return 1;
    }
    displays.close();
    const QByteArray platform = "offscreen:configfile=" + displayConfig.toUtf8();
    qputenv("QT_QPA_PLATFORM", platform);
    QApplication app(argc, argv);
    ConfigTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "config_tests.moc"
