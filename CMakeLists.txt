# SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.30)

set(KF_VERSION "6.31.0") # handled by release scripts
project(SonicLogging VERSION ${KF_VERSION})

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FeatureSummary)
find_package(ECM 6.31.0 NO_MODULE)
set_package_properties(ECM PROPERTIES TYPE REQUIRED DESCRIPTION "Extra CMake Modules." URL "https://github.com/Sonic-DE/sonic-frameworks-cmake-modules")
feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(ECMGenerateExportHeader)
include(ECMSetupVersion)

include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(KDEGitCommitHooks)
include(CMakePackageConfigHelpers)

set(REQUIRED_QT_VERSION 6.11.0)
find_package(Qt6 ${REQUIRED_QT_VERSION} CONFIG REQUIRED Core)

# Where processes running as root write <binary>.log. Packagers may move it.
set(SONIC_LOGGING_ROOT_DIR "/var/log/sonic" CACHE PATH "Directory for log files of programs running as root")

set(soniclogging_version_header "${CMAKE_CURRENT_BINARY_DIR}/src/soniclogging_version.h")
ecm_setup_version(PROJECT VARIABLE_PREFIX SONICLOGGING
                        VERSION_HEADER "${soniclogging_version_header}"
                        PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/SonicLoggingConfigVersion.cmake"
                        SOVERSION 6)

set(SONICLOGGING_INSTALL_INCLUDEDIR "${KDE_INSTALL_INCLUDEDIR}/Sonic/Logging")

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6Test ${REQUIRED_QT_VERSION} CONFIG REQUIRED)
    add_subdirectory(autotests)
endif()

set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/SonicLogging")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SonicLoggingConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SonicLoggingConfig.cmake"
    INSTALL_DESTINATION ${CMAKECONFIG_INSTALL_DIR}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SonicLoggingConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/SonicLoggingConfigVersion.cmake"
        DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
        COMPONENT Devel)

install(EXPORT SonicLoggingTargets DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
        FILE SonicLoggingTargets.cmake NAMESPACE Sonic:: COMPONENT Devel)
install(FILES ${soniclogging_version_header}
        DESTINATION ${SONICLOGGING_INSTALL_INCLUDEDIR} COMPONENT Devel)

include(ECMFeatureSummary)
ecm_feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
