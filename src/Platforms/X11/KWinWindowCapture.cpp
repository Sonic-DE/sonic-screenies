/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "KWinWindowCapture.h"

#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDeadlineTimer>
#include <QFile>

#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace X11 {

CompositorWindowImage captureKWinWindow(xcb_window_t window, bool decorations, bool shadow, bool cursor,
    const QDBusConnection& bus, int timeoutMs)
{
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
        return {{}, QStringLiteral("Could not create the compositor screenshot pipe")};
    }
    QDBusUnixFileDescriptor writeEnd;
    writeEnd.giveFileDescriptor(descriptors[1]);
    QFile readEnd;
    if (!readEnd.open(descriptors[0], QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptors[0]);
        return {{}, QStringLiteral("Could not open the compositor screenshot pipe")};
    }

    const QDeadlineTimer deadline(timeoutMs);
    QDBusReply<QVariantMap> reply;
    {
        auto request = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin.ScreenShot2"),
            QStringLiteral("/org/kde/KWin/ScreenShot2"), QStringLiteral("org.kde.KWin.ScreenShot2"), QStringLiteral("CaptureWindow"));
        const QVariantMap options = {{QStringLiteral("include-decoration"), decorations},
            {QStringLiteral("include-shadow"), shadow}, {QStringLiteral("include-cursor"), cursor},
            {QStringLiteral("native-resolution"), true}};
        request << QString::number(window) << options << QVariant::fromValue(writeEnd);
        reply = bus.call(request, QDBus::Block, timeoutMs);
    }
    // Only the compositor's duplicated write FD may remain open. Otherwise a
    // truncated image could not be detected as EOF while we read the pipe.
    writeEnd = QDBusUnixFileDescriptor();
    if (!reply.isValid()) {
        return {{}, reply.error().name() + QStringLiteral(": ") + reply.error().message()};
    }

    const auto metadata = reply.value();
    const quint64 width = metadata.value(QStringLiteral("width")).toULongLong();
    const quint64 height = metadata.value(QStringLiteral("height")).toULongLong();
    const quint64 stride = metadata.value(QStringLiteral("stride")).toULongLong();
    const auto format = QImage::Format(metadata.value(QStringLiteral("format")).toUInt());
    const qreal scale = metadata.value(QStringLiteral("scale")).toDouble();
    constexpr quint64 maximumBytes = 256 * 1024 * 1024;
    const bool supportedFormat = format == QImage::Format_RGB32 || format == QImage::Format_ARGB32
        || format == QImage::Format_ARGB32_Premultiplied || format == QImage::Format_RGBX8888
        || format == QImage::Format_RGBA8888 || format == QImage::Format_RGBA8888_Premultiplied;
    if (metadata.value(QStringLiteral("type")).toString() != QLatin1String("raw") || !supportedFormat
        || width == 0 || height == 0 || width > 32768 || height > 32768 || stride != width * 4
        || stride * height > maximumBytes || !std::isfinite(scale) || scale <= 0) {
        return {{}, QStringLiteral("Invalid compositor screenshot metadata")};
    }

    QImage image(int(width), int(height), format);
    if (image.isNull() || quint64(image.bytesPerLine()) != stride) {
        return {{}, QStringLiteral("Could not allocate the compositor screenshot")};
    }
    qsizetype received = 0;
    while (received < image.sizeInBytes()) {
        if (deadline.hasExpired()) {
            return {{}, QStringLiteral("Timed out reading the compositor screenshot")};
        }
        const auto count = ::read(readEnd.handle(), image.bits() + received, image.sizeInBytes() - received);
        if (count > 0) {
            received += count;
            continue;
        }
        if (count == 0) {
            return {{}, QStringLiteral("Incomplete compositor screenshot")};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            return {{}, QStringLiteral("Could not read the compositor screenshot")};
        }
        pollfd descriptor{readEnd.handle(), POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, int(deadline.remainingTime()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || deadline.hasExpired()) {
            return {{}, QStringLiteral("Timed out reading the compositor screenshot")};
        }
    }
    image.setDevicePixelRatio(scale);
    return {image, {}};
}

} // namespace X11
