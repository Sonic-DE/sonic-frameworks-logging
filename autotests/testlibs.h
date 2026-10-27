// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

// Small shared libraries the helper links, each logging from its own object.
#define TESTLIB_EXPORT __attribute__((visibility("default")))

TESTLIB_EXPORT void libaWarning(const QString& text);
//! Logs sonicDebug() << ++*evaluations, so a suppressed message leaves the counter alone.
TESTLIB_EXPORT void libaDebug(int* evaluations);
TESTLIB_EXPORT void libbWarning(const QString& text);
TESTLIB_EXPORT void libbDebug(const QString& text);

TESTLIB_EXPORT void libcReverseDomainWarning(const QString& text);
TESTLIB_EXPORT void libcOtherCategoryWarning(const QString& text);
TESTLIB_EXPORT void libcPlainWarning(const QString& text);

//! Debug, info and warning from a category declared in libQt6Fake.so.
TESTLIB_EXPORT void fakeQtMessages(const QString& text);

//! sonicDebug() and sonicInfo() from a library built without QT_NO_DEBUG.
TESTLIB_EXPORT void debugBuildMessages(const QString& text);
//! sonicDebug() and sonicInfo() from a library built with QT_NO_DEBUG.
TESTLIB_EXPORT void releaseBuildMessages(const QString& text);
