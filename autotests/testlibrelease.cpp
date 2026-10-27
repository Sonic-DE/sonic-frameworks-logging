// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "testlibs.h"

#include "log.h"

#ifndef QT_NO_DEBUG
#error "this library must be built with QT_NO_DEBUG"
#endif

SONIC_LOGGING_COMPONENT("org.sonicde.test.release")

void releaseBuildMessages(const QString& text)
{
    sonicDebug().noquote() << text << "debug";
    sonicInfo().noquote() << text << "info";
}
