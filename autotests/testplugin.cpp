// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "logging.h"

// A plugin as a host would load it: the automatic install plus a run-once function.
namespace {
int loads = 0;

void onLoad()
{
    ++loads;
}
}

SONIC_LOGGING_AUTO_INSTALL(onLoad)

extern "C" __attribute__((visibility("default"))) int sonicLoggingTestPluginLoads()
{
    return loads;
}
