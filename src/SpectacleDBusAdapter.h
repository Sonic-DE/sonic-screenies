/* This file is part of Spectacle, the KDE screenshot utility
 * SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "SpectacleCore.h"
#include <QDBusAbstractAdaptor>

class SpectacleDBusAdapter : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.sonicde.Screenies")
public:
    SpectacleDBusAdapter(SpectacleCore *parent);
    ~SpectacleDBusAdapter() override = default;

    inline SpectacleCore *parent() const;

public Q_SLOTS:

    Q_NOREPLY void FullScreen(int includeMousePointer);
    Q_NOREPLY void CurrentScreen(int includeMousePointer);
    Q_NOREPLY void ActiveWindow(int includeWindowDecorations, int includeMousePointer, int includeWindowShadow);
    Q_NOREPLY void WindowUnderCursor(int includeWindowDecorations, int includeMousePointer, int includeWindowShadow);
    Q_NOREPLY void RectangularRegion(int includeMousePointer);
    Q_NOREPLY void RecordRegion(int includeMousePointer);
    Q_NOREPLY void RecordScreen(int includeMousePointer);
    Q_NOREPLY void RecordWindow(int includeMousePointer);
    Q_NOREPLY void OpenWithoutScreenshot();

Q_SIGNALS:

    void ScreenshotTaken(const QString &fileName);
    void ScreenshotFailed(const QString &message);
    void RecordingTaken(const QString &fileName);
    void RecordingFailed(const QString &message);
};

// Compatibility adaptor for scripts using the pre-Screenies capture API.
// Redeclare the slots/signals so Qt exports the legacy interface explicitly.
class LegacySpectacleDBusAdapter final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.Spectacle")
public:
    LegacySpectacleDBusAdapter(SpectacleDBusAdapter* adapter, SpectacleCore* parent)
        : QDBusAbstractAdaptor(parent)
        , m_adapter(adapter)
    {
        setAutoRelaySignals(false);
        connect(adapter, &SpectacleDBusAdapter::ScreenshotTaken, this, &LegacySpectacleDBusAdapter::ScreenshotTaken);
        connect(adapter, &SpectacleDBusAdapter::ScreenshotFailed, this, &LegacySpectacleDBusAdapter::ScreenshotFailed);
        connect(adapter, &SpectacleDBusAdapter::RecordingTaken, this, &LegacySpectacleDBusAdapter::RecordingTaken);
        connect(adapter, &SpectacleDBusAdapter::RecordingFailed, this, &LegacySpectacleDBusAdapter::RecordingFailed);
    }

public Q_SLOTS:
    Q_NOREPLY void FullScreen(int includeMousePointer)
    {
        m_adapter->FullScreen(includeMousePointer);
    }
    Q_NOREPLY void CurrentScreen(int includeMousePointer)
    {
        m_adapter->CurrentScreen(includeMousePointer);
    }
    Q_NOREPLY void ActiveWindow(int includeWindowDecorations, int includeMousePointer, int includeWindowShadow)
    {
        m_adapter->ActiveWindow(includeWindowDecorations, includeMousePointer, includeWindowShadow);
    }
    Q_NOREPLY void WindowUnderCursor(int includeWindowDecorations, int includeMousePointer, int includeWindowShadow)
    {
        m_adapter->WindowUnderCursor(includeWindowDecorations, includeMousePointer, includeWindowShadow);
    }
    Q_NOREPLY void RectangularRegion(int includeMousePointer)
    {
        m_adapter->RectangularRegion(includeMousePointer);
    }
    Q_NOREPLY void RecordRegion(int includeMousePointer)
    {
        m_adapter->RecordRegion(includeMousePointer);
    }
    Q_NOREPLY void RecordScreen(int includeMousePointer)
    {
        m_adapter->RecordScreen(includeMousePointer);
    }
    Q_NOREPLY void RecordWindow(int includeMousePointer)
    {
        m_adapter->RecordWindow(includeMousePointer);
    }
    Q_NOREPLY void OpenWithoutScreenshot()
    {
        m_adapter->OpenWithoutScreenshot();
    }

Q_SIGNALS:
    void ScreenshotTaken(const QString& fileName);
    void ScreenshotFailed(const QString& message);
    void RecordingTaken(const QString& fileName);
    void RecordingFailed(const QString& message);

private:
    SpectacleDBusAdapter* const m_adapter;
};
