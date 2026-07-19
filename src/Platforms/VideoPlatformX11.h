/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include "Platforms/VideoPlatform.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QRect>
#include <QString>
#include <QUrl>
#include <memory>
#include <xcb/xcb.h>

class QMediaCaptureSession;
class QMediaRecorder;
class QAudioBufferInput;
class QVideoFrameInput;
class QThread;

namespace X11 {
class FrameGrabWorker;
class TargetSelector;
struct Target;
}

namespace Spectacle::Audio {
class AudioCaptureWorker;
}
namespace Spectacle::Encoding {
class AnimatedEncoderWorker;
}

namespace Spectacle {

class VideoPlatformX11 : public VideoPlatform {
    Q_OBJECT
public:
    explicit VideoPlatformX11(QObject* parent = nullptr);
    ~VideoPlatformX11() override;

    RecordingModes supportedRecordingModes() const override;
    Formats supportedFormats() const override;
    Format effectivePreferredFormat() const override;
    bool isBusy() const override;

public Q_SLOTS:
    void startRecording(const QUrl& fileUrl, RecordingMode recordingMode, const QVariantMap& options, bool includePointer) override;
    void finishRecording() override;

private Q_SLOTS:
    void onFrameProduced(const QImage& image, qint64 startUs, qint64 endUs);
    void onReadyForVideoFrame();
    void onReadyForAudioBuffer();

private:
    enum class Phase { Idle,
        Selecting,
        Starting,
        Recording,
        Finalizing };
    bool startEncoder(const QUrl& fileUrl, const QRect& captureRect, xcb_window_t windowId = 0);
    bool startAnimatedEncoder(const QUrl& fileUrl, const QRect& captureRect, xcb_window_t windowId = 0);
    void startSelectedTarget(const X11::Target& target);
    void beginTargetRecording(const QRect& captureRect, xcb_window_t windowId = 0);
    QUrl resolveOutputUrl(const QUrl& requested, Format format);
    Format resolveFormat(const QUrl& requested) const;
    void stopFrameSource();
    void stopAudioSource();
    void resetAnimatedEncoder();
    void resetCaptureSession();
    void fail(const QString& message);
    void cancel(const QString& message);
    void complete();
    void shutdownIfNeeded();

    Phase m_phase = Phase::Idle;
    RecordingMode m_activeMode = RecordingMode::NoRecordingModes;
    qint64 m_attemptId = 0;

    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QMediaRecorder> m_recorder;
    std::unique_ptr<QVideoFrameInput> m_videoInput;
    std::unique_ptr<QAudioBufferInput> m_audioInput;
    std::unique_ptr<Audio::AudioCaptureWorker> m_audioWorker;
    std::unique_ptr<X11::FrameGrabWorker> m_frameWorker;
    std::unique_ptr<X11::TargetSelector> m_targetSelector;
    Encoding::AnimatedEncoderWorker* m_animatedEncoder = nullptr;
    std::unique_ptr<QThread> m_animatedThread;
    QUrl m_pendingOutputUrl;
    QUrl m_actualOutputUrl;
    QSize m_frameSize;
    Format m_activeFormat = NoFormat;
    bool m_videoInputReady = true;
    bool m_audioInputReady = true;
    bool m_audioEosSent = false;
    bool m_videoEosSent = false;
    bool m_useAudio = false;
    QByteArray m_pendingAudioPcm;
    qint64 m_pendingAudioStartUs = 0;
    bool m_firstFrameAccepted = false;
    bool m_terminalSignalEmitted = false;
    bool m_pendingIncludePointer = false;
};

} // namespace Spectacle
