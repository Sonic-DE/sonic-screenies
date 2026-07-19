/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>

namespace Spectacle::Encoding {

class AnimatedEncoderWorker : public QObject {
    Q_OBJECT

public:
    enum class Format {
        Gif,
        WebP,
    };
    Q_ENUM(Format)

    struct Options {
        Format format = Format::Gif;
        QUrl outputUrl;
        QSize frameSize;
        int maximumQueuedFrames = 8;
    };

    explicit AnimatedEncoderWorker(QObject* parent = nullptr);
    ~AnimatedEncoderWorker() override;

    static bool isGifSupported();
    static bool isAnimatedWebPSupported();
    static bool isFormatSupported(Format format);
    static QString unavailableReason(Format format);

public Q_SLOTS:
    void start(const Spectacle::Encoding::AnimatedEncoderWorker::Options& options);
    void submitFrame(const QImage& image, qint64 startTimeUs, qint64 endTimeUs);
    void finish();
    void cancel();

Q_SIGNALS:
    void frameDropped(qint64 droppedCount);
    void finished(const QUrl& outputUrl);
    void failed(const QString& message);
    void canceled();

private:
    struct Private;
    Private* const d;
};

} // namespace Spectacle::Encoding

Q_DECLARE_METATYPE(Spectacle::Encoding::AnimatedEncoderWorker::Options)
Q_DECLARE_METATYPE(Spectacle::Encoding::AnimatedEncoderWorker::Format)
