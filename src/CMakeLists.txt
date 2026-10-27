# SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

add_library(SonicLogging)
add_library(Sonic::Logging ALIAS SonicLogging)
set_target_properties(SonicLogging PROPERTIES
    VERSION     ${SONICLOGGING_VERSION}
    SOVERSION   ${SONICLOGGING_SOVERSION}
    EXPORT_NAME Logging
)

target_sources(SonicLogging PRIVATE
    logging.cpp
    logging.h
    logging_p.h
    log.h
)

target_compile_definitions(SonicLogging PRIVATE
    SONIC_LOGGING_ROOT_DIR="${SONIC_LOGGING_ROOT_DIR}"
    _GNU_SOURCE
)

ecm_generate_export_header(SonicLogging
    BASE_NAME SonicLogging
    VERSION ${KF_VERSION}
    # Quoted, so the export header finds the version header installed beside it.
    USE_VERSION_HEADER "\"soniclogging_version.h\""
    VERSION_BASE_NAME SonicLogging
    DEPRECATED_BASE_VERSION 0
)

target_link_libraries(SonicLogging PUBLIC Qt6::Core PRIVATE ${CMAKE_DL_LIBS})

target_include_directories(SonicLogging
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_BINARY_DIR}>"
    INTERFACE "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>"
)

install(TARGETS SonicLogging EXPORT SonicLoggingTargets ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

# <Sonic/Logging/Logging> and <Sonic/Logging/Log> forward to the lowercase headers installed next to them.
install(FILES
    Logging
    Log
    logging.h
    log.h
    ${CMAKE_CURRENT_BINARY_DIR}/soniclogging_export.h
    DESTINATION ${SONICLOGGING_INSTALL_INCLUDEDIR}
    COMPONENT Devel
)
