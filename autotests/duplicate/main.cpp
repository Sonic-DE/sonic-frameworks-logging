// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "log.h"

// Stands in for the library, which isn't built yet when CMake runs this check.
void Sonic::Logging::Internal::registerComponent(const void*, const char*)
{
}

void firstWarning();

int main()
{
    firstWarning();
    return 0;
}
