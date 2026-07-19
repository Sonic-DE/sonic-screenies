/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <memory>

#include <xcb/xcb.h>

#include "X11Connection.h"

class QTimer;
class QElapsedTimer;

namespace X11 {

struct FrameGrabOptions {
    enum class Source {
        Screen,
        Window,
        Region,
    };

    Source source = Source::Screen;
    QRect targetRect; // root-native for Screen; window-relative for Window; logical native for Region
    xcb_window_t windowId = 0;
    QList<QRect> monitorRects; // for Region: native monitor rectangles that intersect the selection
    bool includePointer = false;
    int requestedFps = 30;
};

class FrameGrabWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameGrabWorker(QObject* parent = nullptr);
    ~FrameGrabWorker() override;

public Q_SLOTS:
    void startWorker();
    void stopWorker();

    void setOptions(const FrameGrabOptions& options);
    void startCapture();
    void stopCapture();

    void acknowledgeFrame(bool accepted);

Q_SIGNALS:
    void producerReady();
    void frameProduced(const QImage& image, qint64 startUs, qint64 endUs);
    void producerFailed(const QString& message);
    void producerStopped();

private:
    void timerTick();

    Connection m_connection;
    std::unique_ptr<QTimer> m_timer;
    std::unique_ptr<QElapsedTimer> m_clock;
    FrameGrabOptions m_options;
    bool m_capturing = false;
    bool m_pendingFrame = false;
    qint64 m_nextFrameUs = 0;
    qint64 m_skippedFrames = 0;
};

} // namespace X11
