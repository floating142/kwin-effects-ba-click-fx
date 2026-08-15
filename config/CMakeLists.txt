# SPDX-License-Identifier: GPL-3.0-or-later

# 配置模块编译为独立插件，由 KWin 的桌面特效 KCM 动态加载。

add_library(kwin_ba_click_fx_config MODULE)

target_sources(kwin_ba_click_fx_config PRIVATE
    baclickfxconfig.cpp
)

# uic 在构建目录生成 ui_baclickfxconfig.h；同时包含 src 目录，以便配置模块与特效
# 本体共用 baclickfxdefaults.h 中的配置键和默认值。
target_include_directories(kwin_ba_click_fx_config PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

ki18n_wrap_ui(kwin_ba_click_fx_config baclickfxconfig.ui)

target_link_libraries(kwin_ba_click_fx_config PRIVATE
    Qt6::Core
    # 保存配置后通过 DBus 通知 KWin 热重载，避免依赖发行版特定的 qdbus 可执行文件。
    Qt6::DBus
    Qt6::Widgets
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtils
)

# 安装到 KWin 桌面特效配置插件目录。
install(TARGETS kwin_ba_click_fx_config
        DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs)
