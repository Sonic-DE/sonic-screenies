/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QImage>
#include <QPoint>
#include <xcb/xcb.h>

namespace X11 {

struct WindowImage {
    QImage image;
    QPoint position; // Native root coordinates, including decorations if requested.
};

// Reads only the selected window and its WM frame, never the root drawable.
// An unavailable isolated backing pixmap is a capture failure, not permission
// to substitute a crop of the desktop.
WindowImage captureWindow(xcb_connection_t* connection, xcb_window_t window, bool includeDecorations);

} // namespace X11
