// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "log.h"
#include "soniclogging_export.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

/*!
 * The SonicDE message handler.
 *
 * Every executable writes its own log file: /var/log/sonic/<binary>.log when it
 * runs as root, ~/.local/state/sonic/<binary>.log otherwise. Messages also go to
 * journald when it is running. When a service manager already captures the
 * program's output (systemd, runit, s6, OpenRC's supervise-daemon, dinit, or an
 * X session's ~/.xsession-errors), lines go to that output instead and no file
 * is written.
 *
 * Each line reads "<ISO timestamp with ms> (DD|II|WW|EE) [<source>] <message>",
 * where <source> is the category for the executable's own messages, the
 * library's ID for messages from a library, and the binary name for messages
 * without a category.
 */
namespace Sonic::Logging {
struct Options {
    //! Empty: the name the program was started as, without directories, e.g. "soniclogin-helper".
    QString binaryName{};
    //! Also send to journald when it's running.
    bool journald = true;
    //! Rotate to <file>.last on every run instead of by size.
    bool rotateOnStartup = false;
    //! Mirror lines to stderr when it is a terminal and not already the journal.
    bool mirrorToStderrWhenTty = true;
    QStringList dropDebugCategoryPrefixes = {QStringLiteral("qt."), QStringLiteral("kf."), QStringLiteral("libkwin."), QStringLiteral("kwin_")};
    QStringList dropDebugCategoryExact = {QStringLiteral("kwin"), QStringLiteral("heap"), QString()};
    //! Debug messages containing any of these (case-insensitive) are dropped.
    QStringList dropDebugMessageSubstrings = {QStringLiteral("[rub]"),
        QStringLiteral("QRhiBufferData"),
        QStringLiteral("QRhiTextureData"),
        QStringLiteral("release to pool"),
        QStringLiteral("now has the following listeners"),
        QStringLiteral("ChangeListener listener="),
        QStringLiteral("Generated BaselineJIT code"),
        QStringLiteral("Code at [0x"),
        QStringLiteral("XCB_"),
        QStringLiteral("XInput Event(")};
};

/*!
 * Installs the handler for this program; returns the handler that was in place
 * before any Sonic install. Works before or after the Q*Application exists. A
 * second call returns nullptr and changes nothing.
 */
SONICLOGGING_EXPORT QtMessageHandler install(const Options& options = {});

//! The file chosen by install(); empty before install() and when a service manager captures the output.
SONICLOGGING_EXPORT QString logFilePath();

//! Root only: creates or repairs the root log directory; false, with one stderr line, if it can't.
SONICLOGGING_EXPORT bool prepareRootLogDirectory();

//! The compiled-in root log directory.
SONICLOGGING_EXPORT QString rootLogDirectory();

SONICLOGGING_EXPORT void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message);

namespace Internal {
    //! The automatic install used by SONIC_LOGGING_AUTO_INSTALL(); it never replaces a handler the host installed.
    SONICLOGGING_EXPORT void autoInstall();
}
}

/*!
 * For plugins: installs the handler in the host process, unless the host
 * installed its own. The optional argument is a void() function to run once at
 * the same point, also in plugins loaded after the application exists.
 */
#define SONIC_LOGGING_AUTO_INSTALL(...)                    \
    namespace {                                            \
        void sonicLoggingAutoInstallRun()                  \
        {                                                  \
            static bool done = false;                      \
            if (done) {                                    \
                return;                                    \
            }                                              \
            done = true;                                   \
            Sonic::Logging::Internal::autoInstall();       \
            __VA_OPT__(__VA_ARGS__();)                     \
        }                                                  \
        void sonicLoggingAutoInstallLate()                 \
        {                                                  \
            if (QCoreApplication::instance()) {            \
                sonicLoggingAutoInstallRun();              \
            }                                              \
        }                                                  \
    }                                                      \
    Q_COREAPP_STARTUP_FUNCTION(sonicLoggingAutoInstallRun) \
    Q_CONSTRUCTOR_FUNCTION(sonicLoggingAutoInstallLate)
