/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "X11FrameGrabWorker.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QPainter>
#include <QTimer>

#include <xcb/xcb_image.h>
#include <xcb/xfixes.h>

using namespace Qt::StringLiterals;

namespace X11 {

FrameGrabWorker::FrameGrabWorker(QObject* parent)
    : QObject(parent)
{
}

FrameGrabWorker::~FrameGrabWorker()
{
}

void FrameGrabWorker::startWorker()
{
    if (!m_connection.isValid()) {
        Q_EMIT producerFailed(u"Could not connect to the X11 display"_s);
        return;
    }
    if (!m_clock) {
        m_clock = std::make_unique<QElapsedTimer>();
    }
    if (!m_timer) {
        m_timer = std::make_unique<QTimer>();
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer.get(), &QTimer::timeout, this, &FrameGrabWorker::timerTick);
    }
    Q_EMIT producerReady();
}

void FrameGrabWorker::stopWorker()
{
    if (m_timer) {
        m_timer->stop();
    }
}

void FrameGrabWorker::setOptions(const FrameGrabOptions& options)
{
    m_options = options;
}

void FrameGrabWorker::startCapture()
{
    if (!m_clock || !m_timer) {
        qWarning() << "FrameGrabWorker::startCapture called before startWorker";
        return;
    }
    m_clock->restart();
    m_nextFrameUs = 0;
    m_pendingFrame = false;
    m_skippedFrames = 0;
    const int intervalMs = std::max(1, 1000 / std::max(1, m_options.requestedFps));
    m_timer->start(intervalMs);
    m_capturing = true;
}

void FrameGrabWorker::stopCapture()
{
    m_capturing = false;
    if (m_timer) {
        m_timer->stop();
    }
    Q_EMIT producerStopped();
}

void FrameGrabWorker::acknowledgeFrame(bool accepted)
{
    Q_UNUSED(accepted);
    m_pendingFrame = false;
}

void FrameGrabWorker::timerTick()
{
    if (!m_capturing) {
        return;
    }
    if (m_pendingFrame) {
        ++m_skippedFrames;
        return;
    }
    qint64 elapsedUs = m_clock->nsecsElapsed() / 1000;
    qint64 startUs = m_nextFrameUs;
    if (startUs < elapsedUs) {
        // Late tick; record the slip but reuse it as the next slot to avoid duplicate timestamps.
        startUs = elapsedUs;
    }
    m_nextFrameUs = startUs + (1000000LL / std::max(1, m_options.requestedFps));
    const auto connection = m_connection.handle();
    xcb_drawable_t drawable = m_options.source == FrameGrabOptions::Source::Window && m_options.windowId
        ? m_options.windowId
        : m_connection.rootWindow();
    if (drawable == XCB_NONE) {
        Q_EMIT producerFailed(u"The X11 capture drawable is unavailable"_s);
        stopCapture();
        return;
    }

    QRect rect = m_options.targetRect;
    if (rect.isEmpty()) {
        const auto cookie = xcb_get_geometry(connection, drawable);
        ReplyPtr geometry(xcb_get_geometry_reply(connection, cookie, nullptr));
        const auto reply = geometry.get<xcb_get_geometry_reply_t>();
        if (!reply || reply->width == 0 || reply->height == 0) {
            Q_EMIT producerFailed(u"Could not determine the X11 capture geometry"_s);
            stopCapture();
            return;
        }
        rect = QRect(0, 0, reply->width, reply->height);
    }

    const auto image = xcb_image_get(connection,
        drawable,
        rect.x(),
        rect.y(),
        rect.width(),
        rect.height(),
        ~0u,
        XCB_IMAGE_FORMAT_Z_PIXMAP);
    if (!image || !image->data || image->width == 0 || image->height == 0) {
        if (image) {
            xcb_image_destroy(image);
        }
        Q_EMIT producerFailed(u"X11 returned an empty video frame"_s);
        stopCapture();
        return;
    }

    QImage wrapped(image->data,
        image->width,
        image->height,
        image->stride,
        image->bpp == 32 ? QImage::Format_RGB32 : QImage::Format_RGB888);
    // Deep-copy before destroying the XCB image. convertToFormat() may return a shallow copy when
    // the source already has the requested format.
    QImage frame = wrapped.copy().convertToFormat(QImage::Format_RGB32);
    xcb_image_destroy(image);

    if (m_options.includePointer) {
        const auto cursorCookie = xcb_xfixes_get_cursor_image(connection);
        ReplyPtr cursorReply(xcb_xfixes_get_cursor_image_reply(connection, cursorCookie, nullptr));
        const auto cursor = cursorReply.get<xcb_xfixes_get_cursor_image_reply_t>();
        if (cursor && cursor->width > 0 && cursor->height > 0) {
            QPoint cursorPosition(cursor->x - cursor->xhot, cursor->y - cursor->yhot);
            if (drawable == m_connection.rootWindow()) {
                cursorPosition -= rect.topLeft();
            } else {
                const auto translateCookie = xcb_translate_coordinates(connection,
                    m_connection.rootWindow(),
                    drawable,
                    cursorPosition.x(),
                    cursorPosition.y());
                ReplyPtr translateReply(xcb_translate_coordinates_reply(connection, translateCookie, nullptr));
                if (const auto translated = translateReply.get<xcb_translate_coordinates_reply_t>()) {
                    cursorPosition = QPoint(translated->dst_x, translated->dst_y) - rect.topLeft();
                }
            }
            const auto pixels = xcb_xfixes_get_cursor_image_cursor_image(cursor);
            QImage cursorImage(reinterpret_cast<const uchar*>(pixels),
                cursor->width,
                cursor->height,
                cursor->width * 4,
                QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&frame);
            painter.drawImage(cursorPosition, cursorImage);
        }
    }
    m_pendingFrame = true;
    Q_EMIT frameProduced(frame, startUs, m_nextFrameUs);
}

} // namespace X11
