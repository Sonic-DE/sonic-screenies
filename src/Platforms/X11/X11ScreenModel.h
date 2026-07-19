/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

namespace X11 {

struct MonitorInfo {
    QString name;
    QRect rect;
    bool primary = false;
};

class ScreenModel {
public:
    bool populateFromQScreen();
    const QList<MonitorInfo>& monitors() const
    {
        return m_monitors;
    }
    QRect monitorRectAt(const QPoint& nativePos) const;

private:
    QList<MonitorInfo> m_monitors;
};

} // namespace X11
