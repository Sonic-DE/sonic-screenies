/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "X11ScreenModel.h"

#include <QGuiApplication>
#include <QScreen>

using namespace Qt::StringLiterals;

namespace X11 {

bool ScreenModel::populateFromQScreen()
{
    m_monitors.clear();
    const auto screens = QGuiApplication::screens();
    for (auto screen : screens) {
        MonitorInfo info;
        info.name = screen->name();
        info.rect = screen->geometry();
        info.primary = screen == QGuiApplication::primaryScreen();
        m_monitors.append(info);
    }
    return !m_monitors.isEmpty();
}

QRect ScreenModel::monitorRectAt(const QPoint& nativePos) const
{
    for (const auto& monitor : m_monitors) {
        if (monitor.rect.contains(nativePos)) {
            return monitor.rect;
        }
    }
    return {};
}

} // namespace X11
