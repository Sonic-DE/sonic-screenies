/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QString>

namespace PlatformStartup {

bool isSupportedX11Session(const QString& platformName, const QString& xdgSessionType);

} // namespace PlatformStartup
