// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "log.h"

// Opened with dlopen() while other threads log, so its declaration registers concurrently.
SONIC_LOGGING_COMPONENT("org.sonicde.test.dlopen")

extern "C" __attribute__((visibility("default"))) void sonicLoggingTestDlopenWarning(const char* text)
{
    sonicWarning() << text;
}
