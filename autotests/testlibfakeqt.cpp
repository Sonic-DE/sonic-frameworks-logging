// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "testlibs.h"

#include <QLoggingCategory>

// Built as libQt6Fake.so, so its messages count as Qt's own.
namespace {
const QLoggingCategory fakeQtCategory("fake.qtlib");
}

void fakeQtMessages(const QString& text)
{
    QMessageLogger().debug(fakeQtCategory).noquote() << text << "debug";
    QMessageLogger().info(fakeQtCategory).noquote() << text << "info";
    QMessageLogger().warning(fakeQtCategory).noquote() << text << "warning";
}
