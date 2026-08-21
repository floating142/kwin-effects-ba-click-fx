# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

project(kwin-effects-ba-click-fx VERSION 1.0.1 LANGUAGES CXX)

# 根据源文件内容生成可复现的构建标识。install-local.sh 可通过 /Effects.debug
# 查询该值，以判断 KWin 当前加载的插件是否与本次构建一致。相同源码始终生成相同
# 标识，不依赖编译日期或时间。
file(GLOB_RECURSE BA_CLICK_FX_BUILD_ID_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/shader/*.vert"
    "${CMAKE_SOURCE_DIR}/shader/*.frag"
    "${CMAKE_SOURCE_DIR}/shader/*.glsl"
    "${CMAKE_SOURCE_DIR}/assets/*"
    "${CMAKE_SOURCE_DIR}/config/*.cpp"
    "${CMAKE_SOURCE_DIR}/config/*.h"
    "${CMAKE_SOURCE_DIR}/config/*.ui"
)
set(BA_CLICK_FX_BUILD_ID_MATERIAL "")
foreach(path IN LISTS BA_CLICK_FX_BUILD_ID_FILES)
    file(SHA256 "${path}" file_hash)
    string(APPEND BA_CLICK_FX_BUILD_ID_MATERIAL "${file_hash}")
endforeach()
string(SHA256 BA_CLICK_FX_BUILD_ID_FULL "${BA_CLICK_FX_BUILD_ID_MATERIAL}")
string(SUBSTRING "${BA_CLICK_FX_BUILD_ID_FULL}" 0 12 BA_CLICK_FX_BUILD_ID)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 项目直接使用 ECM 的安装目录变量和 CMake 模块，因此显式加载 ECM 并设置模块路径。
find_package(ECM 6.26.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

# KWin::kwin 的接口依赖包含 Qt6::DBus、Qt6::Quick 和 Qt6::Widgets，而
# KWinConfig.cmake 不会加载其中全部组件，因此需在查找 KWin 前显式加载。
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui DBus Quick Widgets)
# KCMUtils 为配置模块提供 KCModule 基类和 ki18n_wrap_ui。
find_package(KF6 6.26 REQUIRED COMPONENTS Config CoreAddons I18n KCMUtils WindowSystem)

find_package(KWin REQUIRED)

# EffectPluginFactory 的接口标识包含 KWin 版本，升级 KWin 后必须重新编译插件。
# 配置阶段输出目标版本，供文档与安装脚本核对。
message(STATUS "针对 KWin ${KWin_VERSION} 构建（升级 KWin 后需重新编译本插件）")
message(STATUS "BA Click FX build id: ${BA_CLICK_FX_BUILD_ID}")

add_subdirectory(src)
add_subdirectory(config)
ki18n_install(po)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
