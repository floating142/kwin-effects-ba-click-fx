// SPDX-License-Identifier: GPL-3.0-or-later
// BA Click FX 的 KCM 配置模块。

#pragma once

#include <KCModule>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QTimer>

#include "ui_baclickfxconfig.h"

class QDBusPendingCallWatcher;

/// 读写 BA Click FX 配置并通知 KWin 重新加载参数。
class BaClickFxEffectConfig : public KCModule
{
    Q_OBJECT

public:
    explicit BaClickFxEffectConfig(QObject *parent, const KPluginMetaData &data);

public Q_SLOTS:
    void load() override;
    void save() override;
    void defaults() override;

private:
    /// 根据当前滑块值刷新数值标签。
    void updateValueLabels();
    void sendPreview();
    void updateGlobalScaleVisibility();
    void rebuildOutputScaleEditors();
    void refreshOutputMetadata();
    QHash<QString, class QSlider *> m_outputSliders;
    QHash<QString, class QLabel *> m_outputLabels;
    QHash<QString, class QLabel *> m_outputNames;
    QHash<QString, class QToolButton *> m_outputResetButtons;
    QPointer<QWidget> m_outputScaleContainer;
    QSet<QString> m_outputOverrides;
    bool m_outputOverridesReset = false;
    QHash<QString, QString> m_outputIds;
    QHash<QString, QSize> m_outputNativeSizes;
    QTimer m_outputRefreshDebounce;
    QTimer m_outputRefreshTimeout;
    QPointer<QDBusPendingCallWatcher> m_outputRefreshWatcher;

    Ui::BaClickFxEffectConfigForm m_ui;
};
