// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QLoggingCategory>

// A plugin that isn't SonicDE's and declares no component, such as a KDE plugin: its own category's messages
// are kept, while a qt.* category used from it is Qt's.
namespace {
const QLoggingCategory foreignPluginCategory("foreign.plugin");
const QLoggingCategory qtCategoryInPlugin("qt.foreign.plugin");
}

extern "C" __attribute__((visibility("default"))) void sonicLoggingTestQtPluginForeign(const char* text)
{
    QMessageLogger().debug(foreignPluginCategory).noquote() << text << "debug";
    QMessageLogger().info(foreignPluginCategory).noquote() << text << "info";
    QMessageLogger().warning(foreignPluginCategory).noquote() << text << "warning";
    QMessageLogger().debug(qtCategoryInPlugin).noquote() << text << "qt-category debug";
    QMessageLogger().info(qtCategoryInPlugin).noquote() << text << "qt-category info";
    QMessageLogger().warning(qtCategoryInPlugin).noquote() << text << "qt-category warning";
}
