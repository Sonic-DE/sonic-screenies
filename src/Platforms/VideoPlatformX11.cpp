/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "VideoPlatformX11.h"

#include "ExportManager.h"
#include "PlatformOutput.h"
#include "Platforms/Audio/AudioCaptureWorker.h"
#include "Platforms/Encoding/AnimatedEncoderWorker.h"
#include "Platforms/X11/X11FrameGrabWorker.h"
#include "Platforms/X11/X11TargetSelector.h"
#include "settings.h"

#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoFrameInput>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Spectacle {

namespace {
    QMediaFormat mediaFormatFor(VideoPlatform::Format format, bool withAudio = false)
    {
        QMediaFormat mediaFormat;
        if (format == VideoPlatform::MP4_H264) {
            mediaFormat.setFileFormat(QMediaFormat::MPEG4);
            mediaFormat.setVideoCodec(QMediaFormat::VideoCodec::H264);
            if (withAudio) {
                mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::AAC);
            }
        } else {
            mediaFormat.setFileFormat(QMediaFormat::WebM);
            mediaFormat.setVideoCodec(QMediaFormat::VideoCodec::VP9);
            if (withAudio) {
                mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::Opus);
            }
        }
        return mediaFormat;
    }

    bool isQtRecorderFormat(VideoPlatform::Format format)
    {
        return format == VideoPlatform::WebM_VP9 || format == VideoPlatform::MP4_H264;
    }

    bool isAnimatedFormat(VideoPlatform::Format format)
    {
        return format == VideoPlatform::Gif || format == VideoPlatform::WebP;
    }

    bool supportsEncoding(QMediaFormat::FileFormat fileFormat, QMediaFormat::VideoCodec videoCodec)
    {
        QMediaFormat probe;
        if (!probe.supportedFileFormats(QMediaFormat::Encode).contains(fileFormat)) {
            return false;
        }
        probe.setFileFormat(fileFormat);
        return probe.supportedVideoCodecs(QMediaFormat::Encode).contains(videoCodec);
    }
} // namespace

VideoPlatformX11::VideoPlatformX11(QObject* parent)
    : VideoPlatform(parent)
{
    Formats formats;
    if (supportsEncoding(QMediaFormat::WebM, QMediaFormat::VideoCodec::VP9)) {
        formats |= WebM_VP9;
    }
    if (supportsEncoding(QMediaFormat::MPEG4, QMediaFormat::VideoCodec::H264)) {
        formats |= MP4_H264;
    }
    if (Encoding::AnimatedEncoderWorker::isAnimatedWebPSupported()) {
        formats |= WebP;
    }
    if (Encoding::AnimatedEncoderWorker::isGifSupported()) {
        formats |= Gif;
    }
    setSupportedFormats(formats);
    setSupportedRecordingModes(formats ? RecordingModes(Screen | Window | Region) : RecordingModes{});
    setEffectivePreferredFormat(effectivePreferredFormat());
}

VideoPlatformX11::~VideoPlatformX11()
{
    shutdownIfNeeded();
}

VideoPlatform::RecordingModes VideoPlatformX11::supportedRecordingModes() const
{
    return supportedFormats() ? RecordingModes(Screen | Window | Region) : RecordingModes{};
}

VideoPlatform::Formats VideoPlatformX11::supportedFormats() const
{
    Formats formats;
    if (supportsEncoding(QMediaFormat::WebM, QMediaFormat::VideoCodec::VP9)) {
        formats |= WebM_VP9;
    }
    if (supportsEncoding(QMediaFormat::MPEG4, QMediaFormat::VideoCodec::H264)) {
        formats |= MP4_H264;
    }
    if (Encoding::AnimatedEncoderWorker::isAnimatedWebPSupported()) {
        formats |= WebP;
    }
    if (Encoding::AnimatedEncoderWorker::isGifSupported()) {
        formats |= Gif;
    }
    return formats;
}

VideoPlatform::Format VideoPlatformX11::effectivePreferredFormat() const
{
    const auto formats = supportedFormats();
    const auto preferred = static_cast<Format>(Settings::preferredVideoFormat());
    if (formats.testFlag(preferred)) {
        return preferred;
    }
    if (formats.testFlag(WebM_VP9)) {
        return WebM_VP9;
    }
    if (formats.testFlag(MP4_H264)) {
        return MP4_H264;
    }
    if (formats.testFlag(WebP)) {
        return WebP;
    }
    if (formats.testFlag(Gif)) {
        return Gif;
    }
    return NoFormat;
}

bool VideoPlatformX11::isBusy() const
{
    return m_phase != Phase::Idle;
}

VideoPlatform::Format VideoPlatformX11::resolveFormat(const QUrl& requested) const
{
    const auto explicitFormat = requested.isEmpty() ? NoFormat : formatForPath(requested.toLocalFile());
    if (explicitFormat != NoFormat) {
        return supportedFormats().testFlag(explicitFormat) ? explicitFormat : NoFormat;
    }
    return effectivePreferredFormat();
}

QUrl VideoPlatformX11::resolveOutputUrl(const QUrl& requested, Format format)
{
    if (!requested.isEmpty()) {
        return requested.isLocalFile() ? requested : QUrl{};
    }
    auto exportManager = ExportManager::instance();
    exportManager->updateTimestamp();
    const auto filename = ExportManager::formattedFilename(Settings::videoFilenameTemplate(),
        exportManager->timestamp(),
        QString(),
        Settings::videoSaveLocation());
    auto output = exportManager->tempVideoUrl(filename);
    if (!output.isLocalFile()) {
        return {};
    }
    const auto extension = extensionForFormat(format);
    QFileInfo info(output.toLocalFile());
    if (info.suffix().compare(extension, Qt::CaseInsensitive) != 0) {
        output = QUrl::fromLocalFile(info.absolutePath() + u'/' + info.completeBaseName() + u'.' + extension);
    }
    return output;
}

void VideoPlatformX11::startRecording(const QUrl& fileUrl,
    RecordingMode recordingMode,
    const QVariantMap& options,
    bool includePointer)
{
    if (recordingMode == NoRecordingModes) {
        Q_EMIT recordingCanceled(u"Recording canceled: no recording mode"_s);
        return;
    }

    const bool completingRegionSelection = recordingMode == Region
        && m_phase == Phase::Selecting
        && options.contains(u"rect"_s);
    if (m_phase != Phase::Idle && !completingRegionSelection) {
        return;
    }

    if (!completingRegionSelection) {
        ++m_attemptId;
        m_terminalSignalEmitted = false;
        m_pendingOutputUrl = fileUrl;
        m_pendingIncludePointer = includePointer;
        m_activeMode = recordingMode;
        m_phase = Phase::Selecting;
        Q_EMIT busyChanged();
    }

    if (recordingMode == Region && !options.contains(u"rect"_s)) {
        Q_EMIT regionRequested();
        return;
    }

    if (recordingMode == Region) {
        beginTargetRecording(options.value(u"rect"_s).toRectF().toAlignedRect());
        return;
    }

    // A concrete target may be supplied by integration tests or another trusted caller.
    if (options.contains(u"rect"_s)) {
        const auto windowId = static_cast<xcb_window_t>(options.value(u"windowId"_s).toULongLong());
        beginTargetRecording(options.value(u"rect"_s).toRectF().toAlignedRect(), windowId);
        return;
    }

    const auto selectionMode = recordingMode == Screen ? X11::TargetSelectionMode::Screen : X11::TargetSelectionMode::Window;
    m_targetSelector = std::make_unique<X11::TargetSelector>(selectionMode);
    connect(m_targetSelector.get(), &X11::TargetSelector::targetSelected, this, &VideoPlatformX11::startSelectedTarget, Qt::QueuedConnection);
    connect(m_targetSelector.get(), &X11::TargetSelector::selectionCanceled, this, [this](const QString& reason) { cancel(reason.isEmpty() ? u"Recording target selection was canceled"_s : reason); }, Qt::QueuedConnection);
    if (!m_targetSelector->start()) {
        // start() emits the exactly-once cancellation signal on failure.
        if (m_phase != Phase::Idle) {
            cancel(u"Could not start X11 recording target selection"_s);
        }
    }
}

void VideoPlatformX11::startSelectedTarget(const X11::Target& target)
{
    if (m_phase != Phase::Selecting) {
        return;
    }
    const auto windowId = target.mode == X11::TargetSelectionMode::Window ? target.windowId : XCB_NONE;
    QRect captureRect = target.geometry;
    if (windowId != XCB_NONE) {
        captureRect.moveTopLeft(QPoint(0, 0));
    }
    m_targetSelector.reset();
    beginTargetRecording(captureRect, windowId);
}

void VideoPlatformX11::beginTargetRecording(const QRect& requestedRect, xcb_window_t windowId)
{
    QRect captureRect = requestedRect;
    if (captureRect.isEmpty()) {
        fail(u"The selected recording area is empty"_s);
        return;
    }
    captureRect.setWidth(captureRect.width() & ~1);
    captureRect.setHeight(captureRect.height() & ~1);
    if (captureRect.width() < 2 || captureRect.height() < 2) {
        fail(u"The selected recording area is too small"_s);
        return;
    }

    const auto format = resolveFormat(m_pendingOutputUrl);
    if (!isQtRecorderFormat(format) && !isAnimatedFormat(format)) {
        fail(u"The requested video format is not available"_s);
        return;
    }
    const auto output = resolveOutputUrl(m_pendingOutputUrl, format);
    if (output.isEmpty()) {
        fail(u"Could not create a local output file for the recording"_s);
        return;
    }

    m_activeFormat = format;
    m_actualOutputUrl = output;
    m_useAudio = isQtRecorderFormat(format)
        && (Settings::videoRecordMicrophone() || Settings::videoRecordSystemAudio());
    if (isAnimatedFormat(format)) {
        startAnimatedEncoder(output, captureRect, windowId);
    } else {
        startEncoder(output, captureRect, windowId);
    }
}

bool VideoPlatformX11::startEncoder(const QUrl& fileUrl, const QRect& captureRect, xcb_window_t windowId)
{
    m_phase = Phase::Starting;
    m_firstFrameAccepted = false;
    m_videoInputReady = true;
    m_audioInputReady = true;
    m_audioEosSent = false;
    m_videoEosSent = false;
    m_pendingAudioPcm.clear();

    QVideoFrameFormat frameFormat(captureRect.size(), QVideoFrameFormat::Format_YUV420P);
    frameFormat.setStreamFrameRate(30.0);
    m_session = std::make_unique<QMediaCaptureSession>();
    m_recorder = std::make_unique<QMediaRecorder>();
    m_videoInput = std::make_unique<QVideoFrameInput>(frameFormat);
    if (m_useAudio) {
        m_audioInput = std::make_unique<QAudioBufferInput>(Audio::AudioCaptureWorker::audioFormat());
    }
    m_recorder->setOutputLocation(fileUrl);
    m_recorder->setMediaFormat(mediaFormatFor(m_activeFormat, m_useAudio));
    m_recorder->setVideoResolution(captureRect.size());
    m_recorder->setVideoFrameRate(30.0);
    m_recorder->setAutoStop(true);
    m_frameSize = captureRect.size();
    m_session->setRecorder(m_recorder.get());
    m_session->setVideoFrameInput(m_videoInput.get());
    if (m_audioInput) {
        m_session->setAudioBufferInput(m_audioInput.get());
    }

    connect(m_videoInput.get(), &QVideoFrameInput::readyToSendVideoFrame, this, &VideoPlatformX11::onReadyForVideoFrame);
    if (m_audioInput) {
        connect(m_audioInput.get(), &QAudioBufferInput::readyToSendAudioBuffer, this, &VideoPlatformX11::onReadyForAudioBuffer);
        m_audioWorker = std::make_unique<Audio::AudioCaptureWorker>();
        connect(m_audioWorker.get(), &Audio::AudioCaptureWorker::chunkReady, this, &VideoPlatformX11::onReadyForAudioBuffer, Qt::QueuedConnection);
        connect(m_audioWorker.get(), &Audio::AudioCaptureWorker::failed, this, [this](const QString& message) { fail(message); }, Qt::QueuedConnection);
    }
    connect(m_recorder.get(), &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState state) {
        if (state == QMediaRecorder::StoppedState && m_phase == Phase::Finalizing) {
            complete();
        }
    });
    connect(m_recorder.get(), &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error, const QString& errorString) {
        fail(errorString.isEmpty() ? u"Qt Multimedia failed while recording"_s : errorString);
    });

    m_frameWorker = std::make_unique<X11::FrameGrabWorker>();
    X11::FrameGrabOptions grabOptions;
    grabOptions.source = windowId == XCB_NONE
        ? (m_activeMode == Region ? X11::FrameGrabOptions::Source::Region : X11::FrameGrabOptions::Source::Screen)
        : X11::FrameGrabOptions::Source::Window;
    grabOptions.targetRect = captureRect;
    grabOptions.windowId = windowId;
    grabOptions.includePointer = m_pendingIncludePointer;
    grabOptions.requestedFps = 30;
    m_frameWorker->setOptions(grabOptions);
    connect(m_frameWorker.get(), &X11::FrameGrabWorker::frameProduced, this, &VideoPlatformX11::onFrameProduced);
    connect(m_frameWorker.get(), &X11::FrameGrabWorker::producerFailed, this, [this](const QString& message) {
        fail(message);
    });

    setRecordingMode(m_activeMode);
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError) {
        fail(m_recorder->errorString());
        return false;
    }
    if (m_audioWorker) {
        Audio::AudioCaptureWorker::CaptureOptions audioOptions;
        audioOptions.microphone = Settings::videoRecordMicrophone();
        audioOptions.systemAudio = Settings::videoRecordSystemAudio();
        if (!m_audioWorker->startCapture(audioOptions)) {
            fail(u"Could not start the requested PulseAudio recording sources"_s);
            return false;
        }
    }
    m_frameWorker->startWorker();
    m_frameWorker->startCapture();
    return true;
}

bool VideoPlatformX11::startAnimatedEncoder(const QUrl& fileUrl, const QRect& captureRect, xcb_window_t windowId)
{
    m_phase = Phase::Starting;
    m_firstFrameAccepted = false;
    m_frameSize = captureRect.size();

    m_animatedThread = std::make_unique<QThread>();
    m_animatedEncoder = new Encoding::AnimatedEncoderWorker;
    m_animatedEncoder->moveToThread(m_animatedThread.get());
    connect(m_animatedEncoder, &Encoding::AnimatedEncoderWorker::finished, this, [this](const QUrl&) {
        complete();
    });
    connect(m_animatedEncoder, &Encoding::AnimatedEncoderWorker::failed, this, [this](const QString& message) {
        fail(message);
    });
    connect(m_animatedEncoder, &Encoding::AnimatedEncoderWorker::canceled, this, [this] {
        cancel(u"Animated recording was canceled"_s);
    });
    m_animatedThread->start();

    Encoding::AnimatedEncoderWorker::Options encoderOptions;
    encoderOptions.format = m_activeFormat == WebP
        ? Encoding::AnimatedEncoderWorker::Format::WebP
        : Encoding::AnimatedEncoderWorker::Format::Gif;
    encoderOptions.outputUrl = fileUrl;
    encoderOptions.frameSize = captureRect.size();
    QMetaObject::invokeMethod(m_animatedEncoder, [encoder = m_animatedEncoder, encoderOptions] { encoder->start(encoderOptions); }, Qt::BlockingQueuedConnection);

    m_frameWorker = std::make_unique<X11::FrameGrabWorker>();
    X11::FrameGrabOptions grabOptions;
    grabOptions.source = windowId == XCB_NONE
        ? (m_activeMode == Region ? X11::FrameGrabOptions::Source::Region : X11::FrameGrabOptions::Source::Screen)
        : X11::FrameGrabOptions::Source::Window;
    grabOptions.targetRect = captureRect;
    grabOptions.windowId = windowId;
    grabOptions.includePointer = m_pendingIncludePointer;
    grabOptions.requestedFps = 30;
    m_frameWorker->setOptions(grabOptions);
    connect(m_frameWorker.get(), &X11::FrameGrabWorker::frameProduced, this, &VideoPlatformX11::onFrameProduced);
    connect(m_frameWorker.get(), &X11::FrameGrabWorker::producerFailed, this, [this](const QString& message) {
        fail(message);
    });
    setRecordingMode(m_activeMode);
    m_frameWorker->startWorker();
    m_frameWorker->startCapture();
    return true;
}

void VideoPlatformX11::onReadyForAudioBuffer()
{
    if (!m_audioInput || !m_audioWorker || m_audioEosSent) {
        return;
    }
    m_audioInputReady = true;
    while (m_audioInputReady) {
        if (m_pendingAudioPcm.isEmpty()) {
            Audio::TimestampedPcmChunk chunk;
            if (!m_audioWorker->pullChunk(&chunk)) {
                break;
            }
            m_pendingAudioPcm = std::move(chunk.pcm);
            m_pendingAudioStartUs = chunk.startTimeUs;
        }
        QAudioBuffer buffer(m_pendingAudioPcm, Audio::AudioCaptureWorker::audioFormat(), m_pendingAudioStartUs);
        if (!m_audioInput->sendAudioBuffer(buffer)) {
            m_audioInputReady = false;
            break;
        }
        m_pendingAudioPcm.clear();
    }

    if (m_phase == Phase::Finalizing && m_pendingAudioPcm.isEmpty()) {
        Audio::TimestampedPcmChunk remaining;
        if (!m_audioWorker->pullChunk(&remaining)) {
            m_audioEosSent = m_audioInput->sendAudioBuffer({});
            m_audioInputReady = m_audioEosSent;
        }
    }
}

void VideoPlatformX11::onFrameProduced(const QImage& image, qint64 startUs, qint64 endUs)
{
    if (!m_frameWorker || (m_phase != Phase::Starting && m_phase != Phase::Recording)) {
        return;
    }
    if (image.isNull() || image.size() != m_frameSize) {
        m_frameWorker->acknowledgeFrame(false);
        if (image.isNull()) {
            fail(u"The X11 video source produced an empty frame"_s);
        }
        return;
    }

    if (m_animatedEncoder) {
        const QImage frame = image.format() == QImage::Format_RGB32 ? image : image.convertToFormat(QImage::Format_RGB32);
        QMetaObject::invokeMethod(m_animatedEncoder, [encoder = m_animatedEncoder, frame, startUs, endUs] { encoder->submitFrame(frame, startUs, endUs); }, Qt::QueuedConnection);
        m_frameWorker->acknowledgeFrame(true);
        if (!m_firstFrameAccepted) {
            m_firstFrameAccepted = true;
            m_phase = Phase::Recording;
            setRecordingState(RecordingState::Recording);
        }
        return;
    }
    if (!m_videoInput) {
        m_frameWorker->acknowledgeFrame(false);
        fail(u"The video encoder input is unavailable"_s);
        return;
    }

    QVideoFrame frame(QVideoFrameFormat(image.size(), QVideoFrameFormat::Format_YUV420P));
    if (!frame.map(QVideoFrame::WriteOnly)) {
        m_frameWorker->acknowledgeFrame(false);
        fail(u"Qt Multimedia could not map an X11 video frame"_s);
        return;
    }
    auto clampByte = [](int value) {
        return static_cast<uchar>(std::clamp(value, 0, 255));
    };
    for (int y = 0; y < image.height(); ++y) {
        auto yPlane = frame.bits(0) + y * frame.bytesPerLine(0);
        const auto pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int r = qRed(pixels[x]);
            const int g = qGreen(pixels[x]);
            const int b = qBlue(pixels[x]);
            yPlane[x] = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < image.height(); y += 2) {
        auto uPlane = frame.bits(1) + (y / 2) * frame.bytesPerLine(1);
        auto vPlane = frame.bits(2) + (y / 2) * frame.bytesPerLine(2);
        for (int x = 0; x < image.width(); x += 2) {
            int r = 0;
            int g = 0;
            int b = 0;
            int count = 0;
            for (int dy = 0; dy < 2 && y + dy < image.height(); ++dy) {
                const auto pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y + dy));
                for (int dx = 0; dx < 2 && x + dx < image.width(); ++dx) {
                    r += qRed(pixels[x + dx]);
                    g += qGreen(pixels[x + dx]);
                    b += qBlue(pixels[x + dx]);
                    ++count;
                }
            }
            r /= count;
            g /= count;
            b /= count;
            uPlane[x / 2] = clampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vPlane[x / 2] = clampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
    frame.unmap();
    frame.setStartTime(startUs);
    frame.setEndTime(endUs);
    const bool accepted = m_videoInputReady && m_videoInput->sendVideoFrame(frame);
    m_videoInputReady = accepted;
    m_frameWorker->acknowledgeFrame(accepted);
    if (accepted && !m_firstFrameAccepted) {
        m_firstFrameAccepted = true;
        m_phase = Phase::Recording;
        setRecordingState(RecordingState::Recording);
    }
}

void VideoPlatformX11::onReadyForVideoFrame()
{
    m_videoInputReady = true;
    if (m_phase == Phase::Finalizing && m_videoInput && !m_videoEosSent) {
        m_videoEosSent = m_videoInput->sendVideoFrame({});
        m_videoInputReady = m_videoEosSent;
    }
}

void VideoPlatformX11::finishRecording()
{
    if (m_phase != Phase::Recording && m_phase != Phase::Starting) {
        return;
    }
    m_phase = Phase::Finalizing;
    setRecordingState(RecordingState::Rendering);
    const auto attemptId = m_attemptId;
    QTimer::singleShot(30000, this, [this, attemptId] {
        if (m_attemptId == attemptId && m_phase == Phase::Finalizing) {
            fail(u"Timed out while finalizing the recording"_s);
        }
    });
    stopFrameSource();
    if (m_animatedEncoder) {
        QMetaObject::invokeMethod(m_animatedEncoder, [encoder = m_animatedEncoder] { encoder->finish(); }, Qt::QueuedConnection);
        return;
    }
    stopAudioSource();
    if (m_videoInput && !m_videoEosSent) {
        m_videoEosSent = m_videoInput->sendVideoFrame({});
        m_videoInputReady = m_videoEosSent;
    }
    if (m_audioInput) {
        onReadyForAudioBuffer();
    }
    if (m_recorder && m_recorder->recorderState() == QMediaRecorder::StoppedState) {
        complete();
    }
}

void VideoPlatformX11::stopFrameSource()
{
    if (m_frameWorker) {
        m_frameWorker->stopCapture();
        m_frameWorker->stopWorker();
    }
}

void VideoPlatformX11::stopAudioSource()
{
    if (m_audioWorker) {
        m_audioWorker->stopCapture();
    }
}

void VideoPlatformX11::resetAnimatedEncoder()
{
    if (!m_animatedThread) {
        m_animatedEncoder = nullptr;
        return;
    }
    if (m_animatedEncoder) {
        disconnect(m_animatedEncoder, nullptr, this, nullptr);
        QMetaObject::invokeMethod(m_animatedEncoder, [encoder = m_animatedEncoder] { delete encoder; }, Qt::BlockingQueuedConnection);
        m_animatedEncoder = nullptr;
    }
    m_animatedThread->quit();
    m_animatedThread->wait();
    m_animatedThread.reset();
}

void VideoPlatformX11::resetCaptureSession()
{
    stopFrameSource();
    stopAudioSource();
    m_frameWorker.reset();
    m_targetSelector.reset();
    resetAnimatedEncoder();
    if (m_session) {
        m_session->setVideoFrameInput(nullptr);
        m_session->setAudioBufferInput(nullptr);
        m_session->setRecorder(nullptr);
    }
    m_audioInput.reset();
    m_audioWorker.reset();
    m_videoInput.reset();
    m_recorder.reset();
    m_session.reset();
    m_pendingAudioPcm.clear();
}

void VideoPlatformX11::fail(const QString& message)
{
    if (m_phase == Phase::Idle || m_terminalSignalEmitted) {
        return;
    }
    m_terminalSignalEmitted = true;
    m_targetSelector.reset();
    stopFrameSource();
    if (m_recorder && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
        m_recorder->stop();
    }
    PlatformOutput::removeLocalFile(m_actualOutputUrl.toLocalFile());
    resetCaptureSession();
    m_phase = Phase::Idle;
    setRecordingState(RecordingState::NotRecording);
    setRecordingMode(NoRecordingModes);
    Q_EMIT busyChanged();
    Q_EMIT recordingFailed(message);
}

void VideoPlatformX11::cancel(const QString& message)
{
    if (m_phase == Phase::Idle || m_terminalSignalEmitted) {
        return;
    }
    m_terminalSignalEmitted = true;
    PlatformOutput::removeLocalFile(m_actualOutputUrl.toLocalFile());
    resetCaptureSession();
    m_phase = Phase::Idle;
    setRecordingState(RecordingState::NotRecording);
    setRecordingMode(NoRecordingModes);
    Q_EMIT busyChanged();
    Q_EMIT recordingCanceled(message);
}

void VideoPlatformX11::complete()
{
    if (m_phase != Phase::Finalizing || m_terminalSignalEmitted) {
        return;
    }
    const auto output = m_recorder && !m_recorder->actualLocation().isEmpty()
        ? m_recorder->actualLocation()
        : m_actualOutputUrl;
    if (!PlatformOutput::isAcceptableFinalOutput(output)) {
        fail(u"The video recorder did not produce a valid output file"_s);
        return;
    }
    m_terminalSignalEmitted = true;
    resetCaptureSession();
    m_phase = Phase::Idle;
    setRecordingState(RecordingState::Finished);
    setRecordingMode(NoRecordingModes);
    Q_EMIT busyChanged();
    Q_EMIT recordingSaved(output);
}

void VideoPlatformX11::shutdownIfNeeded()
{
    if (m_phase != Phase::Idle) {
        cancel(u"Application shutdown"_s);
    }
}

} // namespace Spectacle

#include "moc_VideoPlatformX11.cpp"
