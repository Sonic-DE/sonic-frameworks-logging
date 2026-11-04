// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "soniclogging_export.h"

#include <QLoggingCategory>
#include <QMessageLogger>

/*!
 * \macro SONIC_LOGGING_COMPONENT(id)
 * \relates Sonic::Logging
 *
 * Logging for SonicDE code.
 *
 * Each library, plugin or executable declares its component once, in exactly
 * one .cpp file:
 *
 * \code
 * SONIC_LOGGING_COMPONENT("org.sonicde.frameworks.ipc");
 * \endcode
 *
 * and then logs anywhere in that binary with sonicDebug(), sonicInfo(),
 * sonicWarning() and sonicCritical(), or sonicFatal() to log a critical line
 * and abort. The component ID \a id is the category name,
 * so the standard rules apply, for example
 * QT_LOGGING_RULES="org.sonicde.frameworks.ipc.debug=true".
 *
 * Debug messages are on by default only when the declaring code is a debug
 * build.
 *
 * A second declaration in the same binary is a duplicate-definition link
 * error, and logging without any declaration is an undefined-symbol link
 * error. Code compiled into several binaries never declares a component.
 */

/*!
 * \macro sonicDebug()
 * \relates Sonic::Logging
 *
 * Returns a QDebug stream for a debug message under the binary's component,
 * declared with SONIC_LOGGING_COMPONENT(). Nothing is formatted when debug
 * messages are off for the component.
 */

/*!
 * \macro sonicInfo()
 * \relates Sonic::Logging
 *
 * Returns a QDebug stream for an info message under the binary's component,
 * declared with SONIC_LOGGING_COMPONENT().
 */

/*!
 * \macro sonicWarning()
 * \relates Sonic::Logging
 *
 * Returns a QDebug stream for a warning under the binary's component,
 * declared with SONIC_LOGGING_COMPONENT().
 */

/*!
 * \macro sonicCritical()
 * \relates Sonic::Logging
 *
 * Returns a QDebug stream for a critical message under the binary's
 * component, declared with SONIC_LOGGING_COMPONENT().
 */

/*!
 * \macro sonicFatal()
 * \relates Sonic::Logging
 *
 * Logs a critical line under the component, then aborts; logging rules can't
 * turn it off.
 */

namespace Sonic::Logging::Internal {
//! Records which loaded object declared \a id; \a address is any address inside it.
SONICLOGGING_EXPORT void registerComponent(const void* address, const char* id);
}

//! Debug messages are on by default only when the declaring code is a debug build.
#ifdef QT_NO_DEBUG
#define SONIC_LOGGING_MIN_LEVEL QtInfoMsg
#else
#define SONIC_LOGGING_MIN_LEVEL QtDebugMsg
#endif

//! The declaring binary's component; hidden, so every shared object binds to its own.
__attribute__((visibility("hidden"))) const QLoggingCategory& sonicLoggingComponent();

#define SONIC_LOGGING_COMPONENT(id)                                                                                     \
    __attribute__((visibility("hidden"))) const QLoggingCategory& sonicLoggingComponent()                               \
    {                                                                                                                   \
        static const QLoggingCategory category(id, SONIC_LOGGING_MIN_LEVEL);                                            \
        return category;                                                                                                \
    }                                                                                                                   \
    namespace {                                                                                                         \
        const struct SonicLoggingComponentRegistrar {                                                                   \
            SonicLoggingComponentRegistrar()                                                                            \
            {                                                                                                           \
                Sonic::Logging::Internal::registerComponent(reinterpret_cast<const void*>(&sonicLoggingComponent), id); \
            }                                                                                                           \
        } sonicLoggingComponentRegistrar;                                                                               \
    }

#define SONIC_LOGGING_MESSAGE(isEnabled, level)                                                                            \
    for (bool sonicLoggingEnabled = sonicLoggingComponent().isEnabled(); sonicLoggingEnabled; sonicLoggingEnabled = false) \
    QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, sonicLoggingComponent().categoryName()).level()

#define sonicDebug() SONIC_LOGGING_MESSAGE(isDebugEnabled, debug)
#define sonicInfo() SONIC_LOGGING_MESSAGE(isInfoEnabled, info)
#define sonicWarning() SONIC_LOGGING_MESSAGE(isWarningEnabled, warning)
#define sonicCritical() SONIC_LOGGING_MESSAGE(isCriticalEnabled, critical)
#define sonicFatal() \
    QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, sonicLoggingComponent().categoryName()).fatal()
