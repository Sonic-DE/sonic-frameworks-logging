// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "testlibs.h"

#include "log.h"

#ifdef QT_NO_DEBUG
#error "this library must be built without QT_NO_DEBUG"
#endif

SONIC_LOGGING_COMPONENT("org.sonicde.test.debug")

void debugBuildMessages(const QString& text)
{
    sonicDebug().noquote() << text << "debug";
    sonicInfo().noquote() << text << "info";
}
