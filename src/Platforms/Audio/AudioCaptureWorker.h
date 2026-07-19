/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include "AudioPcmMixer.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QVector>
#include <QtMultimedia/qaudioformat.h>

#include <atomic>
#include <memory>
#include <vector>

struct pa_context;
struct pa_stream;
struct pa_threaded_mainloop;

namespace Spectacle::Audio {

class AudioCaptureWorker : public QObject {
    Q_OBJECT

public:
    struct CaptureOptions {
        bool microphone = false;
        bool systemAudio = false;
        qint64 maxQueuedFrames = AudioPcmMixer::DefaultMaxQueuedFrames;
        qint64 chunkFrames = AudioPcmMixer::DefaultChunkFrames;
    };

    explicit AudioCaptureWorker(QObject* parent = nullptr);
    ~AudioCaptureWorker() override;

    [[nodiscard]] static QAudioFormat audioFormat();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool pullChunk(TimestampedPcmChunk* chunk);
    [[nodiscard]] int activeSourceCount() const;
    [[nodiscard]] QVector<QByteArray> activeSourceNames() const;

public Q_SLOTS:
    bool startCapture(const CaptureOptions& options);
    void stopCapture();

Q_SIGNALS:
    void captureStarted();
    void chunkReady();
    void captureStopped();
    void failed(const QString& message);

private:
    struct StreamHandle;

    bool waitForContextReady(QString* errorMessage);
    bool waitForStreamReady(pa_stream* stream, QString* errorMessage);
    bool discoverSources(const CaptureOptions& options, QVector<QByteArray>* sourceNames, QString* errorMessage);
    bool createStream(const QByteArray& sourceName, int mixerIndex, QString* errorMessage);
    bool validateSourceExists(const QByteArray& sourceName, QString* errorMessage);
    void cleanupPulse();
    void handleStreamReadable(StreamHandle* handle);
    void handleStreamStateChanged(StreamHandle* handle);
    void handleStreamMoved(StreamHandle* handle);
    void handleStreamSuspended(StreamHandle* handle);
    void drainMixerToOutputQueue();
    void postFailure(const QString& message);
    void emitFailureNow(const QString& message);

    static void contextStateCallback(pa_context* context, void* userdata);
    static void streamReadCallback(pa_stream* stream, size_t nbytes, void* userdata);
    static void streamStateCallback(pa_stream* stream, void* userdata);
    static void streamMovedCallback(pa_stream* stream, void* userdata);
    static void streamSuspendedCallback(pa_stream* stream, void* userdata);

    pa_threaded_mainloop* m_mainloop = nullptr;
    pa_context* m_context = nullptr;
    std::vector<std::unique_ptr<StreamHandle>> m_streams;
    QVector<QByteArray> m_activeSourceNames;

    AudioPcmMixer m_mixer;
    mutable QMutex m_outputMutex;
    QQueue<TimestampedPcmChunk> m_outputQueue;
    qint64 m_outputQueuedFrames = 0;
    qint64 m_maxQueuedFrames = AudioPcmMixer::DefaultMaxQueuedFrames;
    qint64 m_chunkFrames = AudioPcmMixer::DefaultChunkFrames;

    std::atomic_bool m_running = false;
    std::atomic_bool m_callbackActive = false;
    std::atomic_bool m_failurePosted = false;
};

} // namespace Spectacle::Audio

Q_DECLARE_METATYPE(Spectacle::Audio::AudioCaptureWorker::CaptureOptions)
