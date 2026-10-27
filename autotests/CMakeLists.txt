# SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

# Exactly one SONIC_LOGGING_COMPONENT per binary: a second one must fail to link.
set(duplicate_includes "${PROJECT_SOURCE_DIR}/src;${PROJECT_BINARY_DIR}/src")
try_compile(SONIC_LOGGING_SINGLE_DECLARATION_LINKS
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/duplicate/first.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/duplicate/main.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${duplicate_includes}"
    LINK_LIBRARIES Qt6::Core
    CXX_STANDARD 20
    OUTPUT_VARIABLE single_declaration_output
)
if(NOT SONIC_LOGGING_SINGLE_DECLARATION_LINKS)
    message(FATAL_ERROR "A binary with one SONIC_LOGGING_COMPONENT failed to link:\n${single_declaration_output}")
endif()
try_compile(SONIC_LOGGING_DUPLICATE_DECLARATION_LINKS
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/duplicate/first.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/duplicate/second.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/duplicate/main.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${duplicate_includes}"
    LINK_LIBRARIES Qt6::Core
    CXX_STANDARD 20
)
if(SONIC_LOGGING_DUPLICATE_DECLARATION_LINKS)
    message(FATAL_ERROR "A binary with two SONIC_LOGGING_COMPONENT declarations linked; it must not")
endif()

# Test libraries, each logging from its own object.
add_library(sonicloggingtestliba SHARED testliba.cpp)
add_library(sonicloggingtestlibb SHARED testlibb.cpp)
# Debug output on by default whatever the build type, so the suppression rows test the rules alone.
target_compile_options(sonicloggingtestliba PRIVATE -UQT_NO_DEBUG)
target_compile_options(sonicloggingtestlibb PRIVATE -UQT_NO_DEBUG)

add_library(sonicloggingtestlibc SHARED testlibc.cpp)
# Named by file, without a "lib" prefix, for the file-name attribution row.
set_target_properties(sonicloggingtestlibc PROPERTIES PREFIX "")

add_library(sonicloggingtestfakeqt SHARED testlibfakeqt.cpp)
set_target_properties(sonicloggingtestfakeqt PROPERTIES PREFIX "" OUTPUT_NAME libQt6Fake)

add_library(sonicloggingtestdebug SHARED testlibdebug.cpp)
target_compile_options(sonicloggingtestdebug PRIVATE -UQT_NO_DEBUG)
add_library(sonicloggingtestrelease SHARED testlibrelease.cpp)
target_compile_definitions(sonicloggingtestrelease PRIVATE QT_NO_DEBUG)

foreach(testlib sonicloggingtestliba sonicloggingtestlibb sonicloggingtestlibc sonicloggingtestfakeqt sonicloggingtestdebug sonicloggingtestrelease)
    target_link_libraries(${testlib} PRIVATE Sonic::Logging Qt6::Core)
endforeach()

add_library(sonicloggingtestplugin MODULE testplugin.cpp)
target_link_libraries(sonicloggingtestplugin PRIVATE Sonic::Logging Qt6::Core)

add_library(sonicloggingtestdlopen MODULE testlibdlopen.cpp)
target_link_libraries(sonicloggingtestdlopen PRIVATE Sonic::Logging Qt6::Core)

# Stand-ins for a SonicDE plugin and a plugin from someone else (e.g. KDE), both loaded with dlopen().
add_library(sonicloggingtestqtpluginsonic MODULE testqtpluginsonic.cpp)
add_library(sonicloggingtestqtpluginforeign MODULE testqtpluginforeign.cpp)
foreach(testplugin sonicloggingtestqtpluginsonic sonicloggingtestqtpluginforeign)
    target_link_libraries(${testplugin} PRIVATE Sonic::Logging Qt6::Core)
endforeach()

add_executable(loggingtesthelper loggingtesthelper.cpp)
target_compile_definitions(loggingtesthelper PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE:sonicloggingtestplugin>"
    TEST_DLOPEN_PATH="$<TARGET_FILE:sonicloggingtestdlopen>"
    TEST_QT_PLUGIN_SONIC_PATH="$<TARGET_FILE:sonicloggingtestqtpluginsonic>"
    TEST_QT_PLUGIN_FOREIGN_PATH="$<TARGET_FILE:sonicloggingtestqtpluginforeign>"
)
target_link_libraries(loggingtesthelper PRIVATE
    Sonic::Logging
    Qt6::Core
    sonicloggingtestliba
    sonicloggingtestlibb
    sonicloggingtestlibc
    sonicloggingtestfakeqt
    sonicloggingtestdebug
    sonicloggingtestrelease
    ${CMAKE_DL_LIBS}
)
add_dependencies(loggingtesthelper sonicloggingtestplugin sonicloggingtestdlopen sonicloggingtestqtpluginsonic sonicloggingtestqtpluginforeign)

add_executable(loggingtest loggingtest.cpp)
target_compile_definitions(loggingtest PRIVATE HELPER_PATH="$<TARGET_FILE:loggingtesthelper>")
target_link_libraries(loggingtest PRIVATE Sonic::Logging Qt6::Test)
add_dependencies(loggingtest loggingtesthelper)
add_test(NAME loggingtest COMMAND loggingtest)
