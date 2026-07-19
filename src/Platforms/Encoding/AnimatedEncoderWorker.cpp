/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "AnimatedEncoderWorker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQueue>

#include <algorithm>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

using namespace Qt::StringLiterals;

namespace Spectacle::Encoding {
namespace {

    constexpr AVRational microsecondTimeBase{1, 1000000};
    constexpr AVRational codecTimeBase{1, 1000};

    struct CodecSpec {
        const char* muxerName = nullptr;
        const char* encoderName = nullptr;
        AVCodecID codecId = AV_CODEC_ID_NONE;
    };

    CodecSpec codecSpecFor(AnimatedEncoderWorker::Format format)
    {
        if (format == AnimatedEncoderWorker::Format::WebP) {
            return {"webp", "libwebp_anim", AV_CODEC_ID_WEBP};
        }
        return {"gif", nullptr, AV_CODEC_ID_GIF};
    }

    QString ffmpegError(int code)
    {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(code, buffer, sizeof(buffer));
        return QString::fromUtf8(buffer);
    }

    QString localPathForUrl(const QUrl& url)
    {
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }

    const AVCodec* findEncoder(AnimatedEncoderWorker::Format format)
    {
        const auto spec = codecSpecFor(format);
        if (spec.encoderName) {
            const AVCodec* codec = avcodec_find_encoder_by_name(spec.encoderName);
            if (!codec || codec->id != spec.codecId) {
                return nullptr;
            }
            return codec;
        }
        return avcodec_find_encoder(spec.codecId);
    }

    bool hasMuxer(AnimatedEncoderWorker::Format format)
    {
        return av_guess_format(codecSpecFor(format).muxerName, nullptr, nullptr) != nullptr;
    }

    bool pixelFormatSupported(const AVCodec* codec, AVPixelFormat format)
    {
        if (!codec->pix_fmts) {
            return true;
        }
        for (const AVPixelFormat* it = codec->pix_fmts; *it != AV_PIX_FMT_NONE; ++it) {
            if (*it == format) {
                return true;
            }
        }
        return false;
    }

    AVPixelFormat choosePixelFormat(const AVCodec* codec, AnimatedEncoderWorker::Format format)
    {
        const AVPixelFormat webpPreferred[] = {
            AV_PIX_FMT_BGRA,
            AV_PIX_FMT_RGBA,
            AV_PIX_FMT_RGB24,
            AV_PIX_FMT_YUVA420P,
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_NONE,
        };
        const AVPixelFormat gifPreferred[] = {
            AV_PIX_FMT_RGB8,
            AV_PIX_FMT_BGR8,
            AV_PIX_FMT_PAL8,
            AV_PIX_FMT_RGB24,
            AV_PIX_FMT_NONE,
        };
        const auto* preferred = format == AnimatedEncoderWorker::Format::WebP ? webpPreferred : gifPreferred;
        for (const AVPixelFormat* it = preferred; *it != AV_PIX_FMT_NONE; ++it) {
            if (pixelFormatSupported(codec, *it)) {
                return *it;
            }
        }
        return codec->pix_fmts ? codec->pix_fmts[0] : AV_PIX_FMT_NONE;
    }

    QString unavailableReasonFor(AnimatedEncoderWorker::Format format)
    {
        if (!hasMuxer(format)) {
            return format == AnimatedEncoderWorker::Format::WebP
                ? u"The FFmpeg WebP muxer is not available"_s
                : u"The FFmpeg GIF muxer is not available"_s;
        }
        if (!findEncoder(format)) {
            return format == AnimatedEncoderWorker::Format::WebP
                ? u"The FFmpeg libwebp_anim encoder is not available"_s
                : u"The FFmpeg GIF encoder is not available"_s;
        }
        return {};
    }

    struct FormatContextDeleter {
        void operator()(AVFormatContext* context) const
        {
            if (context) {
                avformat_free_context(context);
            }
        }
    };

    struct CodecContextDeleter {
        void operator()(AVCodecContext* context) const
        {
            avcodec_free_context(&context);
        }
    };

    struct FrameDeleter {
        void operator()(AVFrame* frame) const
        {
            av_frame_free(&frame);
        }
    };

    struct PacketDeleter {
        void operator()(AVPacket* packet) const
        {
            av_packet_free(&packet);
        }
    };

    struct SwsContextDeleter {
        void operator()(SwsContext* context) const
        {
            sws_freeContext(context);
        }
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

} // namespace

struct AnimatedEncoderWorker::Private {
    struct QueuedFrame {
        QImage image;
        qint64 startTimeUs = 0;
        qint64 endTimeUs = 0;
    };

    AnimatedEncoderWorker* q = nullptr;
    Options options;
    QQueue<QueuedFrame> queue;
    FormatContextPtr formatContext;
    CodecContextPtr codecContext;
    SwsContextPtr swsContext;
    AVStream* stream = nullptr;
    const AVCodec* codec = nullptr;
    AVPixelFormat outputPixelFormat = AV_PIX_FMT_NONE;
    QString outputPath;
    bool ioOpen = false;
    bool started = false;
    bool finishing = false;
    bool terminalEmitted = false;
    bool processQueued = false;
    bool wroteFrame = false;
    qint64 lastSubmittedStartUs = -1;
    qint64 lastSubmittedEndUs = -1;
    qint64 lastEncodedPts = -1;
    qint64 droppedCount = 0;

    explicit Private(AnimatedEncoderWorker* parent)
        : q(parent)
    {
    }

    void reset()
    {
        closeIo();
        queue.clear();
        swsContext.reset();
        codecContext.reset();
        formatContext.reset();
        stream = nullptr;
        codec = nullptr;
        outputPixelFormat = AV_PIX_FMT_NONE;
        outputPath.clear();
        ioOpen = false;
        started = false;
        finishing = false;
        processQueued = false;
        wroteFrame = false;
        lastSubmittedStartUs = -1;
        lastSubmittedEndUs = -1;
        lastEncodedPts = -1;
        droppedCount = 0;
    }

    void closeIo()
    {
        if (formatContext && ioOpen) {
            avio_closep(&formatContext->pb);
            ioOpen = false;
        }
    }

    void removeIncompleteFile()
    {
        closeIo();
        if (!outputPath.isEmpty()) {
            QFile::remove(outputPath);
        }
    }

    void emitFailed(const QString& message)
    {
        if (terminalEmitted) {
            return;
        }
        terminalEmitted = true;
        removeIncompleteFile();
        reset();
        Q_EMIT q->failed(message);
    }

    void emitCanceled()
    {
        if (terminalEmitted) {
            return;
        }
        terminalEmitted = true;
        removeIncompleteFile();
        reset();
        Q_EMIT q->canceled();
    }

    void emitFinished(const QUrl& outputUrl)
    {
        if (terminalEmitted) {
            return;
        }
        terminalEmitted = true;
        reset();
        Q_EMIT q->finished(outputUrl);
    }

    bool configure(const Options& newOptions, QString* error)
    {
        if (started) {
            *error = u"The animated encoder is already running"_s;
            return false;
        }
        if (!newOptions.outputUrl.isLocalFile()) {
            *error = u"Animated recording output must be a local file"_s;
            return false;
        }
        if (newOptions.frameSize.isEmpty()) {
            *error = u"Animated recording frame size is empty"_s;
            return false;
        }
        if (newOptions.frameSize.width() <= 0 || newOptions.frameSize.height() <= 0) {
            *error = u"Animated recording frame size is invalid"_s;
            return false;
        }
        const auto reason = unavailableReasonFor(newOptions.format);
        if (!reason.isEmpty()) {
            *error = reason;
            return false;
        }

        options = newOptions;
        options.maximumQueuedFrames = std::max(1, options.maximumQueuedFrames);
        outputPath = localPathForUrl(options.outputUrl);
        QFileInfo info(outputPath);
        if (!info.absoluteDir().exists()) {
            *error = u"Animated recording output directory does not exist"_s;
            return false;
        }

        codec = findEncoder(options.format);
        outputPixelFormat = choosePixelFormat(codec, options.format);
        if (outputPixelFormat == AV_PIX_FMT_NONE) {
            *error = u"The animated encoder does not expose a usable pixel format"_s;
            return false;
        }

        AVFormatContext* rawFormatContext = nullptr;
        int result = avformat_alloc_output_context2(&rawFormatContext, nullptr, codecSpecFor(options.format).muxerName, outputPath.toUtf8().constData());
        if (result < 0 || !rawFormatContext) {
            *error = u"Could not create animated recording container: %1"_s.arg(ffmpegError(result));
            return false;
        }
        formatContext.reset(rawFormatContext);

        stream = avformat_new_stream(formatContext.get(), nullptr);
        if (!stream) {
            *error = u"Could not create animated recording stream"_s;
            return false;
        }
        stream->id = static_cast<int>(formatContext->nb_streams - 1);
        stream->time_base = codecTimeBase;

        codecContext.reset(avcodec_alloc_context3(codec));
        if (!codecContext) {
            *error = u"Could not allocate animated encoder context"_s;
            return false;
        }
        codecContext->codec_id = codec->id;
        codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
        codecContext->width = options.frameSize.width();
        codecContext->height = options.frameSize.height();
        codecContext->pix_fmt = outputPixelFormat;
        codecContext->time_base = codecTimeBase;
        codecContext->framerate = AVRational{30, 1};
        codecContext->gop_size = 1;
        if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
            codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (options.format == Format::WebP) {
            av_opt_set(codecContext->priv_data, "lossless", "1", 0);
        }

        result = avcodec_open2(codecContext.get(), codec, nullptr);
        if (result < 0) {
            *error = u"Could not open animated encoder: %1"_s.arg(ffmpegError(result));
            return false;
        }
        result = avcodec_parameters_from_context(stream->codecpar, codecContext.get());
        if (result < 0) {
            *error = u"Could not copy animated encoder parameters: %1"_s.arg(ffmpegError(result));
            return false;
        }
        stream->time_base = codecContext->time_base;

        swsContext.reset(sws_getContext(options.frameSize.width(),
            options.frameSize.height(),
            AV_PIX_FMT_BGRA,
            options.frameSize.width(),
            options.frameSize.height(),
            outputPixelFormat,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr));
        if (!swsContext) {
            *error = u"Could not create animated recording color converter"_s;
            return false;
        }

        if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
            result = avio_open(&formatContext->pb, outputPath.toUtf8().constData(), AVIO_FLAG_WRITE);
            if (result < 0) {
                *error = u"Could not open animated recording output: %1"_s.arg(ffmpegError(result));
                return false;
            }
            ioOpen = true;
        }
        result = avformat_write_header(formatContext.get(), nullptr);
        if (result < 0) {
            *error = u"Could not write animated recording header: %1"_s.arg(ffmpegError(result));
            return false;
        }

        started = true;
        return true;
    }

    FramePtr makeFrame(const QueuedFrame& queued, QString* error)
    {
        FramePtr frame(av_frame_alloc());
        if (!frame) {
            *error = u"Could not allocate animated recording frame"_s;
            return nullptr;
        }
        frame->format = outputPixelFormat;
        frame->width = options.frameSize.width();
        frame->height = options.frameSize.height();
        int result = av_frame_get_buffer(frame.get(), 32);
        if (result < 0) {
            *error = u"Could not allocate animated recording frame buffer: %1"_s.arg(ffmpegError(result));
            return nullptr;
        }
        result = av_frame_make_writable(frame.get());
        if (result < 0) {
            *error = u"Animated recording frame buffer is not writable: %1"_s.arg(ffmpegError(result));
            return nullptr;
        }

        const QImage source = queued.image.convertToFormat(QImage::Format_RGB32);
        const uint8_t* sourceData[] = {source.constBits(), nullptr, nullptr, nullptr};
        const int sourceStride[] = {static_cast<int>(source.bytesPerLine()), 0, 0, 0};
        const int converted = sws_scale(swsContext.get(), sourceData, sourceStride, 0, options.frameSize.height(), frame->data, frame->linesize);
        if (converted != options.frameSize.height()) {
            *error = u"Could not convert animated recording frame"_s;
            return nullptr;
        }

        qint64 pts = av_rescale_q(queued.startTimeUs, microsecondTimeBase, codecContext->time_base);
        if (pts <= lastEncodedPts) {
            pts = lastEncodedPts + 1;
        }
        const qint64 scaledEndPts = av_rescale_q(queued.endTimeUs, microsecondTimeBase, codecContext->time_base);
        const qint64 endPts = std::max<qint64>(pts + 1, scaledEndPts);
        frame->pts = pts;
        frame->duration = std::max<qint64>(1, endPts - pts);
        lastEncodedPts = pts;
        return frame;
    }

    bool writeAvailablePackets(QString* error)
    {
        PacketPtr packet(av_packet_alloc());
        if (!packet) {
            *error = u"Could not allocate animated recording packet"_s;
            return false;
        }

        while (true) {
            int result = avcodec_receive_packet(codecContext.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                *error = u"Could not receive animated recording packet: %1"_s.arg(ffmpegError(result));
                return false;
            }
            av_packet_rescale_ts(packet.get(), codecContext->time_base, stream->time_base);
            packet->stream_index = stream->index;
            result = av_interleaved_write_frame(formatContext.get(), packet.get());
            av_packet_unref(packet.get());
            if (result < 0) {
                *error = u"Could not write animated recording packet: %1"_s.arg(ffmpegError(result));
                return false;
            }
            wroteFrame = true;
        }
    }

    bool encodeQueuedFrame(const QueuedFrame& queued, QString* error)
    {
        FramePtr frame = makeFrame(queued, error);
        if (!frame) {
            return false;
        }
        int result = avcodec_send_frame(codecContext.get(), frame.get());
        if (result < 0) {
            *error = u"Could not send animated recording frame: %1"_s.arg(ffmpegError(result));
            return false;
        }
        return writeAvailablePackets(error);
    }

    bool flush(QString* error)
    {
        int result = avcodec_send_frame(codecContext.get(), nullptr);
        if (result < 0 && result != AVERROR_EOF) {
            *error = u"Could not flush animated recording encoder: %1"_s.arg(ffmpegError(result));
            return false;
        }
        if (!writeAvailablePackets(error)) {
            return false;
        }
        result = av_write_trailer(formatContext.get());
        if (result < 0) {
            *error = u"Could not write animated recording trailer: %1"_s.arg(ffmpegError(result));
            return false;
        }
        closeIo();
        if (!wroteFrame || QFileInfo(outputPath).size() <= 0) {
            *error = u"The animated encoder produced an empty output file"_s;
            return false;
        }
        return true;
    }

    void scheduleProcess()
    {
        if (processQueued || !started) {
            return;
        }
        processQueued = true;
        QMetaObject::invokeMethod(q, [this] { processQueuedFrames(); }, Qt::QueuedConnection);
    }

    void processQueuedFrames()
    {
        processQueued = false;
        if (!started || terminalEmitted) {
            return;
        }

        while (!queue.isEmpty()) {
            QString error;
            const auto frame = queue.dequeue();
            if (!encodeQueuedFrame(frame, &error)) {
                emitFailed(error);
                return;
            }
        }

        if (finishing) {
            QString error;
            if (!flush(&error)) {
                emitFailed(error);
                return;
            }
            const auto finishedUrl = options.outputUrl;
            emitFinished(finishedUrl);
        }
    }
};

AnimatedEncoderWorker::AnimatedEncoderWorker(QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
    qRegisterMetaType<Spectacle::Encoding::AnimatedEncoderWorker::Options>();
}

AnimatedEncoderWorker::~AnimatedEncoderWorker()
{
    if (d->started && !d->terminalEmitted) {
        d->removeIncompleteFile();
    }
    delete d;
}

bool AnimatedEncoderWorker::isGifSupported()
{
    return unavailableReasonFor(Format::Gif).isEmpty();
}

bool AnimatedEncoderWorker::isAnimatedWebPSupported()
{
    return unavailableReasonFor(Format::WebP).isEmpty();
}

bool AnimatedEncoderWorker::isFormatSupported(Format format)
{
    return unavailableReasonFor(format).isEmpty();
}

QString AnimatedEncoderWorker::unavailableReason(Format format)
{
    return unavailableReasonFor(format);
}

void AnimatedEncoderWorker::start(const Options& options)
{
    if (d->terminalEmitted) {
        d->terminalEmitted = false;
    }
    QString error;
    if (!d->configure(options, &error)) {
        d->emitFailed(error);
    }
}

void AnimatedEncoderWorker::submitFrame(const QImage& image, qint64 startTimeUs, qint64 endTimeUs)
{
    if (!d->started || d->finishing || d->terminalEmitted) {
        return;
    }
    if (image.isNull() || image.size() != d->options.frameSize) {
        d->emitFailed(u"Animated recording frame size changed or frame is empty"_s);
        return;
    }
    if (image.format() != QImage::Format_RGB32) {
        d->emitFailed(u"Animated recording frames must use QImage::Format_RGB32"_s);
        return;
    }
    if (startTimeUs < 0 || endTimeUs <= startTimeUs) {
        d->emitFailed(u"Animated recording frame timestamps are invalid"_s);
        return;
    }
    if (startTimeUs <= d->lastSubmittedStartUs || endTimeUs <= d->lastSubmittedEndUs) {
        d->emitFailed(u"Animated recording frame timestamps are not monotonic"_s);
        return;
    }

    d->lastSubmittedStartUs = startTimeUs;
    d->lastSubmittedEndUs = endTimeUs;
    if (d->queue.size() >= d->options.maximumQueuedFrames) {
        ++d->droppedCount;
        Q_EMIT frameDropped(d->droppedCount);
        return;
    }
    d->queue.enqueue(Private::QueuedFrame{image.copy(), startTimeUs, endTimeUs});
    d->scheduleProcess();
}

void AnimatedEncoderWorker::finish()
{
    if (!d->started || d->terminalEmitted) {
        return;
    }
    d->finishing = true;
    d->scheduleProcess();
}

void AnimatedEncoderWorker::cancel()
{
    if (!d->started || d->terminalEmitted) {
        return;
    }
    d->emitCanceled();
}

} // namespace Spectacle::Encoding

#include "moc_AnimatedEncoderWorker.cpp"
