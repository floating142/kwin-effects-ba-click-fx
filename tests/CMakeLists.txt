# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(ba_click_fx_logic_tests
    logic_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/curveutils.cpp
    ${CMAKE_SOURCE_DIR}/src/trailstream.cpp
    ${CMAKE_SOURCE_DIR}/src/clickinstance.cpp
    ${CMAKE_SOURCE_DIR}/src/subsystems.cpp
    ${CMAKE_SOURCE_DIR}/src/damageutils.cpp
    ${CMAKE_SOURCE_DIR}/src/pathresampler.cpp
)

target_include_directories(ba_click_fx_logic_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ba_click_fx_logic_tests PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME ba_click_fx_logic_tests COMMAND ba_click_fx_logic_tests)
