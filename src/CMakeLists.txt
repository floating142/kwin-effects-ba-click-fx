# SPDX-License-Identifier: GPL-3.0-or-later

add_library(kwin4_effect_ba_click_fx MODULE)

target_compile_definitions(kwin4_effect_ba_click_fx PRIVATE
    BA_CLICK_FX_BUILD_ID="${BA_CLICK_FX_BUILD_ID}"
    BA_CLICK_FX_KWIN_VERSION="${KWin_VERSION}"
)

# KWin 6.8 为默认 API；只有旧版本启用适配，版本宏仅供 kwincompat 使用。
# 6.6 从 prePaintScreen 参数获取时间戳，且没有 pointerMotion 回调。
if(KWin_VERSION VERSION_LESS "6.7.0")
    target_compile_definitions(kwin4_effect_ba_click_fx PRIVATE BA_CLICK_FX_KWIN_LEGACY_PREPAINT)
endif()

# 6.6/6.7 的 paintScreen 返回 void；6.8 默认传播 bool 绘制结果。
if(KWin_VERSION VERSION_LESS "6.8.0")
    target_compile_definitions(kwin4_effect_ba_click_fx PRIVATE BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN)
endif()

# KWIN_EFFECT_FACTORY_* 宏展开后才包含 Q_OBJECT。将这些宏注册给 AutoMoc，确保
# main.cpp 能够生成并包含对应的元对象代码。
set_property(TARGET kwin4_effect_ba_click_fx PROPERTY AUTOMOC_MACRO_NAMES
    Q_OBJECT Q_GADGET Q_NAMESPACE Q_NAMESPACE_EXPORT
    KWIN_EFFECT_FACTORY
    KWIN_EFFECT_FACTORY_ENABLED
    KWIN_EFFECT_FACTORY_SUPPORTED
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED
)

target_sources(kwin4_effect_ba_click_fx PRIVATE
    baclickfxeffect.cpp
    kwincompat.cpp
    applicationfilter.cpp
    gpurenderer.cpp
    gputexture.cpp
    clickinstance.cpp
    trailstream.cpp
    curveutils.cpp
    diagnostics.cpp
    damageutils.cpp
    bloomutils.cpp
    pathresampler.cpp
    subsystems.cpp
    meshprofiles.cpp
    main.cpp
)

target_link_libraries(kwin4_effect_ba_click_fx PRIVATE
    KWin::kwin
    Qt6::Core
    Qt6::Gui
    Qt6::OpenGL
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::I18n
)

install(TARGETS kwin4_effect_ba_click_fx
        DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)

# 将配置阶段解析出的安装目录写入构建目录，供 install-local.sh 生成环境提示。
# KDE_INSTALL_PLUGINDIR 不是缓存变量，安装脚本无法直接从 CMakeCache.txt 读取。
file(WRITE ${CMAKE_BINARY_DIR}/plugin-install-dir.txt "${KDE_INSTALL_FULL_PLUGINDIR}")
file(WRITE ${CMAKE_BINARY_DIR}/data-install-dir.txt "${KDE_INSTALL_FULL_DATADIR}")

# 安装四张纹理和 Cylinder002.obj；运行时通过 QStandardPaths::GenericDataLocation
# 定位这些资源。
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/
        DESTINATION ${KDE_INSTALL_DATADIR}/kwin/effects/ba-click-fx/assets
        FILES_MATCHING PATTERN "*.png" PATTERN "*.obj")

# 着色器以独立资源安装，仅在效果初始化时读取和编译。
install(DIRECTORY ${CMAKE_SOURCE_DIR}/shader/
        DESTINATION ${KDE_INSTALL_DATADIR}/kwin/effects/ba-click-fx/shader
        FILES_MATCHING PATTERN "*.vert" PATTERN "*.frag" PATTERN "*.glsl")
