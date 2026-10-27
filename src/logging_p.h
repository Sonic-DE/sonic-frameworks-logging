// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "soniclogging_export.h"

#include <QByteArray>
#include <QString>

// Not installed: hooks for the autotests. Each test process sets them before install().
namespace Sonic::Logging::Internal {
struct TestConfig {
    QString fsRoot; //!< prefix for the root log directory and its parent
    QString procRoot; //!< replaces /proc in capture detection
    qint64 parentPid = -1; //!< replaces getppid() when >= 0
    QString stateDir; //!< replaces GenericStateLocation when non-empty
    int isRoot = -1; //!< 0 or 1 replaces geteuid() == 0
    qint64 trustedUid = -1; //!< replaces uid 0 in the ownership checks when >= 0
    qint64 parentTrustedUid = -1; //!< replaces the trusted uid for the parent directory check when >= 0
    QString journalSocketPath; //!< replaces /run/systemd/journal/socket
};

SONICLOGGING_EXPORT void setForTesting(const TestConfig& config);
SONICLOGGING_EXPORT void advanceClockForTesting(qint64 msecs);
SONICLOGGING_EXPORT void setSetuidForTesting(bool setuid);
//! File writes fail with this errno; 0 turns it off.
SONICLOGGING_EXPORT void setWriteErrorForTesting(int error);
//! Sleeps this long inside the sinks while holding the write lock.
SONICLOGGING_EXPORT void setSinkDelayForTesting(int msecs);
//! journald sends fail after this many successful ones; -1 turns it off.
SONICLOGGING_EXPORT void failJournalAfterForTesting(int sends);
SONICLOGGING_EXPORT void forceRotationForTesting();
SONICLOGGING_EXPORT int dladdrCallsForTesting();

SONICLOGGING_EXPORT QString idFromLibraryFileName(const QString& path);
SONICLOGGING_EXPORT QByteArray encodeJournalField(const QByteArray& key, const QByteArray& value);
SONICLOGGING_EXPORT QString cleanBinaryName(const QString& name);
}
