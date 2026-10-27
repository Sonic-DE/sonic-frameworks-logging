// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "testlibs.h"

#include "log.h"

SONIC_LOGGING_COMPONENT("org.sonicde.test.libb")

void libbWarning(const QString& text)
{
    sonicWarning().noquote() << text;
}

void libbDebug(const QString& text)
{
    sonicDebug().noquote() << text;
}
