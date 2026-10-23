/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QDBusConnection>
#include <QImage>
#include <QString>
#include <xcb/xcb.h>

namespace X11 {

struct CompositorWindowImage {
    QImage image;
    QString error;
};

// Blocking D-Bus/pipe transport: call from a worker, never the GUI thread.
// The compositor renders the isolated window, including decorations that do
// not exist in its XComposite backing pixmap. Never substitutes a desktop crop.
CompositorWindowImage captureKWinWindow(xcb_window_t window, bool decorations, bool shadow, bool cursor,
    const QDBusConnection& bus = QDBusConnection::sessionBus(), int timeoutMs = 30000);

} // namespace X11
