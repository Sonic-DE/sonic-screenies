/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "PlatformStartup.h"

using namespace Qt::StringLiterals;

namespace PlatformStartup {

bool isSupportedX11Session(const QString& platformName, const QString& xdgSessionType)
{
    if (platformName != u"xcb"_s) {
        return false;
    }
    const auto sessionType = xdgSessionType.toLower();
    if (sessionType.isEmpty() || sessionType == u"x11"_s) {
        return true;
    }
    return false;
}

} // namespace PlatformStartup
