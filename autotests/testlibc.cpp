// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "testlibs.h"

#include <QLoggingCategory>

// No SONIC_LOGGING_COMPONENT: attribution falls back to the category name or the file name.
// Plain Qt categories are what non-SonicDE libraries use, which is what these rows test.
namespace {
const QLoggingCategory reverseDomainCategory("org.example.test.lib");
const QLoggingCategory otherCategory("kf.example");
}

void libcReverseDomainWarning(const QString& text)
{
    QMessageLogger().warning(reverseDomainCategory).noquote() << text;
}

void libcOtherCategoryWarning(const QString& text)
{
    QMessageLogger().warning(otherCategory).noquote() << text;
}

void libcPlainWarning(const QString& text)
{
    qWarning().noquote() << text;
}
