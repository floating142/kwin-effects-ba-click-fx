# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(ba_click_fx_logic_tests
    logic_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/applicationfilter.cpp
    ${CMAKE_SOURCE_DIR}/src/curveutils.cpp
    ${CMAKE_SOURCE_DIR}/src/trailstream.cpp
    ${CMAKE_SOURCE_DIR}/src/clickinstance.cpp
    ${CMAKE_SOURCE_DIR}/src/subsystems.cpp
    ${CMAKE_SOURCE_DIR}/src/damageutils.cpp
    ${CMAKE_SOURCE_DIR}/src/bloomutils.cpp
    ${CMAKE_SOURCE_DIR}/src/pathresampler.cpp
    ${CMAKE_SOURCE_DIR}/src/meshprofiles.cpp
    ${CMAKE_SOURCE_DIR}/src/diagnostics.cpp
)

target_include_directories(ba_click_fx_logic_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ba_click_fx_logic_tests PRIVATE
    BA_CLICK_FX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(ba_click_fx_logic_tests PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME ba_click_fx_logic_tests COMMAND ba_click_fx_logic_tests)

# 设置页测试使用独立 D-Bus 会话和临时配置，不向宿主 KWin 发送预览。
find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
if(DBUS_RUN_SESSION_EXECUTABLE)
    add_executable(ba_click_fx_config_tests config_tests.cpp)
    target_compile_definitions(ba_click_fx_config_tests PRIVATE
        BA_CLICK_FX_CONFIG_PLUGIN_PATH="$<TARGET_FILE:kwin_ba_click_fx_config>"
    )
    target_link_libraries(ba_click_fx_config_tests PRIVATE
        Qt6::Test Qt6::Widgets Qt6::DBus KF6::ConfigCore KF6::CoreAddons KF6::KCMUtils
    )
    add_dependencies(ba_click_fx_config_tests kwin_ba_click_fx_config)
    add_test(NAME ba_click_fx_config_tests
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:ba_click_fx_config_tests>)
    set_tests_properties(ba_click_fx_config_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# KWin 6.8 的 GLES 存储格式回归需实际 GL 上下文验证；无 EGL 时仍可运行纯逻辑测试。
if(KWin_VERSION VERSION_GREATER_EQUAL "6.8.0")
    find_package(OpenGL QUIET COMPONENTS EGL)
    if(TARGET OpenGL::EGL)
        add_executable(ba_click_fx_gpu_tests
            gpu_tests.cpp
            ${CMAKE_SOURCE_DIR}/src/gputexture.cpp
            ${CMAKE_SOURCE_DIR}/src/gpurenderer.cpp
            ${CMAKE_SOURCE_DIR}/src/curveutils.cpp
            ${CMAKE_SOURCE_DIR}/src/clickinstance.cpp
            ${CMAKE_SOURCE_DIR}/src/trailstream.cpp
            ${CMAKE_SOURCE_DIR}/src/subsystems.cpp
            ${CMAKE_SOURCE_DIR}/src/meshprofiles.cpp
            ${CMAKE_SOURCE_DIR}/src/bloomutils.cpp
        )
        target_include_directories(ba_click_fx_gpu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_compile_definitions(ba_click_fx_gpu_tests PRIVATE
            BA_CLICK_FX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(ba_click_fx_gpu_tests PRIVATE KWin::kwin Qt6::Test OpenGL::EGL)
        add_test(NAME ba_click_fx_gpu_tests COMMAND ba_click_fx_gpu_tests)
        set_tests_properties(ba_click_fx_gpu_tests PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
    endif()
endif()
