// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "log.h"

// A second declaration in the same binary must not link.
SONIC_LOGGING_COMPONENT("org.sonicde.test.second")
