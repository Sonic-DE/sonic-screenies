/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <algorithm>

namespace ScreenshotTiming {

// Hiding a window is asynchronous: X11 compositors may continue painting its
// hide animation after it has been unmapped. Use the same settling interval for
// immediate screenshots as for hiding the UI at the end of a countdown.
inline constexpr int windowHideDelay = 500;

inline constexpr int captureDelay(int requestedDelay, bool isX11)
{
    return std::max(requestedDelay, isX11 ? windowHideDelay : 0);
}

} // namespace ScreenshotTiming
