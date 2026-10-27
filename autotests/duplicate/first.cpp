// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "log.h"

SONIC_LOGGING_COMPONENT("org.sonicde.test.first")

void firstWarning()
{
    sonicWarning() << "first";
}
