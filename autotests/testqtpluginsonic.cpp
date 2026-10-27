// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "log.h"

// A SonicDE plugin, loaded with dlopen() like a KCM or backend.
SONIC_LOGGING_COMPONENT("org.sonicde.test.qtplugin")

extern "C" __attribute__((visibility("default"))) void sonicLoggingTestQtPluginSonic(const char* text)
{
    sonicDebug().noquote() << text << "debug";
    sonicInfo().noquote() << text << "info";
    sonicWarning().noquote() << text << "warning";
}
