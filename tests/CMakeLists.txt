# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(ba_click_fx_logic_tests
    logic_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/input/applicationfilter.cpp
    ${CMAKE_SOURCE_DIR}/src/particles/curveutils.cpp
    ${CMAKE_SOURCE_DIR}/src/particles/trailstream.cpp
    ${CMAKE_SOURCE_DIR}/src/particles/clickinstance.cpp
    ${CMAKE_SOURCE_DIR}/src/particles/subsystems.cpp
    ${CMAKE_SOURCE_DIR}/src/render/damageutils.cpp
    ${CMAKE_SOURCE_DIR}/src/render/bloomutils.cpp
    ${CMAKE_SOURCE_DIR}/src/render/meshprofiles.cpp
    ${CMAKE_SOURCE_DIR}/src/effect/diagnostics.cpp
)

target_include_directories(ba_click_fx_logic_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ba_click_fx_logic_tests PRIVATE
    BA_CLICK_FX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(ba_click_fx_logic_tests PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME ba_click_fx_logic_tests COMMAND ba_click_fx_logic_tests)

# 只核对已提交的数据表，不读取游戏安装，也不触发原函数/GPU 专项构建。
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ba_click_fx_unity_parameters
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/import-unity-touch.py --check)
endif()

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

# GPU 回归会再次编译渲染器；日常只构建逻辑和设置页测试，需要时显式启用。
if(BA_CLICK_FX_BUILD_GPU_TESTS AND KWin_VERSION VERSION_GREATER_EQUAL "6.8.0")
    find_package(OpenGL REQUIRED COMPONENTS EGL)
    if(TARGET OpenGL::EGL)
        add_executable(ba_click_fx_gpu_tests
            gpu_tests.cpp
            ${CMAKE_SOURCE_DIR}/src/render/gputexture.cpp
            ${CMAKE_SOURCE_DIR}/src/render/gpurenderer.cpp
            ${CMAKE_SOURCE_DIR}/src/particles/curveutils.cpp
            ${CMAKE_SOURCE_DIR}/src/particles/clickinstance.cpp
            ${CMAKE_SOURCE_DIR}/src/particles/trailstream.cpp
            ${CMAKE_SOURCE_DIR}/src/particles/subsystems.cpp
            ${CMAKE_SOURCE_DIR}/src/render/meshprofiles.cpp
            ${CMAKE_SOURCE_DIR}/src/render/bloomutils.cpp
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
