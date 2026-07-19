/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "AudioCaptureWorker.h"

#include <QMetaObject>

#include <pulse/pulseaudio.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Spectacle::Audio {

namespace {
    struct ServerInfoResult {
        pa_threaded_mainloop* mainloop = nullptr;
        QByteArray defaultSourceName;
        QByteArray defaultSinkName;
        bool done = false;
        bool failed = false;
    };

    struct SinkInfoResult {
        pa_threaded_mainloop* mainloop = nullptr;
        QByteArray monitorSourceName;
        bool done = false;
        bool failed = false;
    };

    struct SourceInfoResult {
        pa_threaded_mainloop* mainloop = nullptr;
        bool exists = false;
        bool done = false;
        bool failed = false;
    };

    void serverInfoCallback(pa_context* context, const pa_server_info* info, void* userdata)
    {
        auto result = static_cast<ServerInfoResult*>(userdata);
        if (!info) {
            result->failed = true;
        } else {
            result->defaultSourceName = info->default_source_name ? QByteArray(info->default_source_name) : QByteArray();
            result->defaultSinkName = info->default_sink_name ? QByteArray(info->default_sink_name) : QByteArray();
        }
        result->done = true;
        Q_UNUSED(context)
        pa_threaded_mainloop_signal(result->mainloop, 0);
    }

    void sinkInfoCallback(pa_context* context, const pa_sink_info* info, int eol, void* userdata)
    {
        auto result = static_cast<SinkInfoResult*>(userdata);
        if (eol < 0) {
            result->failed = true;
            result->done = true;
        } else if (eol > 0) {
            result->done = true;
        } else if (info) {
            result->monitorSourceName = info->monitor_source_name ? QByteArray(info->monitor_source_name) : QByteArray();
        }
        if (result->done) {
            Q_UNUSED(context)
            pa_threaded_mainloop_signal(result->mainloop, 0);
        }
    }

    void sourceInfoCallback(pa_context* context, const pa_source_info* info, int eol, void* userdata)
    {
        auto result = static_cast<SourceInfoResult*>(userdata);
        if (eol < 0) {
            result->failed = true;
            result->done = true;
        } else if (eol > 0) {
            result->done = true;
        } else if (info) {
            result->exists = true;
        }
        if (result->done) {
            Q_UNUSED(context)
            pa_threaded_mainloop_signal(result->mainloop, 0);
        }
    }

    QString pulseError(pa_context* context, const QString& prefix)
    {
        const int error = context ? pa_context_errno(context) : PA_ERR_INTERNAL;
        return prefix + u": "_s + QString::fromUtf8(pa_strerror(error));
    }
} // namespace

struct AudioCaptureWorker::StreamHandle {
    AudioCaptureWorker* worker = nullptr;
    pa_stream* stream = nullptr;
    int mixerIndex = -1;
    QByteArray sourceName;
    bool active = false;
};

AudioCaptureWorker::AudioCaptureWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<Spectacle::Audio::AudioCaptureWorker::CaptureOptions>();
}

AudioCaptureWorker::~AudioCaptureWorker()
{
    stopCapture();
}

QAudioFormat AudioCaptureWorker::audioFormat()
{
    QAudioFormat format;
    format.setSampleFormat(QAudioFormat::Float);
    format.setSampleRate(AudioPcmMixer::SampleRate);
    format.setChannelCount(AudioPcmMixer::Channels);
    return format;
}

bool AudioCaptureWorker::isRunning() const
{
    return m_running.load(std::memory_order_acquire);
}

bool AudioCaptureWorker::pullChunk(TimestampedPcmChunk* chunk)
{
    if (!chunk) {
        return false;
    }
    QMutexLocker locker(&m_outputMutex);
    if (m_outputQueue.isEmpty()) {
        return false;
    }
    *chunk = m_outputQueue.dequeue();
    m_outputQueuedFrames -= chunk->frameCount;
    return true;
}

int AudioCaptureWorker::activeSourceCount() const
{
    return m_activeSourceNames.size();
}

QVector<QByteArray> AudioCaptureWorker::activeSourceNames() const
{
    return m_activeSourceNames;
}

bool AudioCaptureWorker::startCapture(const CaptureOptions& options)
{
    if (isRunning()) {
        return true;
    }
    if (!options.microphone && !options.systemAudio) {
        emitFailureNow(u"Audio capture requested with no microphone or system source enabled"_s);
        return false;
    }

    stopCapture();
    m_failurePosted.store(false, std::memory_order_release);
    m_maxQueuedFrames = std::max<qint64>(1, options.maxQueuedFrames);
    m_chunkFrames = std::max<qint64>(1, options.chunkFrames);
    {
        QMutexLocker locker(&m_outputMutex);
        m_outputQueue.clear();
        m_outputQueuedFrames = 0;
    }

    QString errorMessage;
    m_mainloop = pa_threaded_mainloop_new();
    if (!m_mainloop) {
        emitFailureNow(u"Could not create the PulseAudio threaded mainloop"_s);
        return false;
    }
    if (pa_threaded_mainloop_start(m_mainloop) < 0) {
        cleanupPulse();
        emitFailureNow(u"Could not start the PulseAudio threaded mainloop"_s);
        return false;
    }

    pa_threaded_mainloop_lock(m_mainloop);
    m_context = pa_context_new(pa_threaded_mainloop_get_api(m_mainloop), "Spectacle X11 recorder");
    if (!m_context) {
        errorMessage = u"Could not create the PulseAudio context"_s;
    } else {
        pa_context_set_state_callback(m_context, &AudioCaptureWorker::contextStateCallback, this);
        if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            errorMessage = pulseError(m_context, u"Could not connect to PulseAudio"_s);
        } else if (!waitForContextReady(&errorMessage)) {
            // waitForContextReady filled errorMessage.
        } else {
            QVector<QByteArray> sourceNames;
            if (discoverSources(options, &sourceNames, &errorMessage)) {
                m_mixer.reset(sourceNames.size(), m_maxQueuedFrames);
                m_activeSourceNames = sourceNames;
                m_callbackActive.store(true, std::memory_order_release);
                for (int i = 0; i < sourceNames.size(); ++i) {
                    if (!createStream(sourceNames[i], i, &errorMessage)) {
                        break;
                    }
                }
            }
        }
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    if (!errorMessage.isEmpty()) {
        cleanupPulse();
        emitFailureNow(errorMessage);
        return false;
    }

    m_running.store(true, std::memory_order_release);
    Q_EMIT captureStarted();
    return true;
}

void AudioCaptureWorker::stopCapture()
{
    const bool wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
    m_callbackActive.store(false, std::memory_order_release);
    cleanupPulse();
    drainMixerToOutputQueue();
    m_mixer.reset();
    m_activeSourceNames.clear();
    if (wasRunning) {
        Q_EMIT captureStopped();
    }
}

bool AudioCaptureWorker::waitForContextReady(QString* errorMessage)
{
    while (true) {
        const auto state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_READY) {
            return true;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            if (errorMessage) {
                *errorMessage = pulseError(m_context, u"PulseAudio context failed"_s);
            }
            return false;
        }
        pa_threaded_mainloop_wait(m_mainloop);
    }
}

bool AudioCaptureWorker::waitForStreamReady(pa_stream* stream, QString* errorMessage)
{
    while (true) {
        const auto state = pa_stream_get_state(stream);
        if (state == PA_STREAM_READY) {
            return true;
        }
        if (!PA_STREAM_IS_GOOD(state)) {
            if (errorMessage) {
                *errorMessage = pulseError(m_context, u"PulseAudio record stream failed"_s);
            }
            return false;
        }
        pa_threaded_mainloop_wait(m_mainloop);
    }
}

bool AudioCaptureWorker::discoverSources(const CaptureOptions& options, QVector<QByteArray>* sourceNames, QString* errorMessage)
{
    ServerInfoResult serverInfo;
    serverInfo.mainloop = m_mainloop;
    auto operation = pa_context_get_server_info(m_context, &serverInfoCallback, &serverInfo);
    if (!operation) {
        *errorMessage = pulseError(m_context, u"Could not query PulseAudio server defaults"_s);
        return false;
    }
    while (!serverInfo.done) {
        pa_threaded_mainloop_wait(m_mainloop);
    }
    pa_operation_unref(operation);
    if (serverInfo.failed) {
        *errorMessage = u"PulseAudio did not provide server default source information"_s;
        return false;
    }

    QVector<QByteArray> requestedNames;
    if (options.microphone) {
        if (serverInfo.defaultSourceName.isEmpty()) {
            *errorMessage = u"PulseAudio has no default microphone source"_s;
            return false;
        }
        requestedNames.push_back(serverInfo.defaultSourceName);
    }

    if (options.systemAudio) {
        if (serverInfo.defaultSinkName.isEmpty()) {
            *errorMessage = u"PulseAudio has no default sink for system audio"_s;
            return false;
        }
        SinkInfoResult sinkInfo;
        sinkInfo.mainloop = m_mainloop;
        operation = pa_context_get_sink_info_by_name(m_context, serverInfo.defaultSinkName.constData(), &sinkInfoCallback, &sinkInfo);
        if (!operation) {
            *errorMessage = pulseError(m_context, u"Could not query the default PulseAudio sink"_s);
            return false;
        }
        while (!sinkInfo.done) {
            pa_threaded_mainloop_wait(m_mainloop);
        }
        pa_operation_unref(operation);
        if (sinkInfo.failed || sinkInfo.monitorSourceName.isEmpty()) {
            *errorMessage = u"PulseAudio default sink has no monitor source for system audio"_s;
            return false;
        }
        requestedNames.push_back(sinkInfo.monitorSourceName);
    }

    for (const auto& name : std::as_const(requestedNames)) {
        if (!sourceNames->contains(name)) {
            if (!validateSourceExists(name, errorMessage)) {
                return false;
            }
            sourceNames->push_back(name);
        }
    }

    if (sourceNames->isEmpty()) {
        *errorMessage = u"PulseAudio source discovery produced no recordable sources"_s;
        return false;
    }
    return true;
}

bool AudioCaptureWorker::createStream(const QByteArray& sourceName, int mixerIndex, QString* errorMessage)
{
    pa_sample_spec sampleSpec;
    sampleSpec.format = PA_SAMPLE_FLOAT32NE;
    sampleSpec.rate = AudioPcmMixer::SampleRate;
    sampleSpec.channels = AudioPcmMixer::Channels;

    pa_channel_map channelMap;
    pa_channel_map_init_stereo(&channelMap);

    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = uint32_t(AudioPcmMixer::DefaultMaxQueuedFrames * AudioPcmMixer::BytesPerFrame);
    bufferAttr.tlength = uint32_t(-1);
    bufferAttr.prebuf = uint32_t(-1);
    bufferAttr.minreq = uint32_t(-1);
    bufferAttr.fragsize = uint32_t(m_chunkFrames * AudioPcmMixer::BytesPerFrame);

    auto handle = std::make_unique<StreamHandle>();
    handle->worker = this;
    handle->mixerIndex = mixerIndex;
    handle->sourceName = sourceName;
    handle->active = true;
    handle->stream = pa_stream_new(m_context, "Spectacle record input", &sampleSpec, &channelMap);
    if (!handle->stream) {
        *errorMessage = pulseError(m_context, u"Could not create a PulseAudio record stream"_s);
        return false;
    }

    pa_stream_set_read_callback(handle->stream, &AudioCaptureWorker::streamReadCallback, handle.get());
    pa_stream_set_state_callback(handle->stream, &AudioCaptureWorker::streamStateCallback, handle.get());
    pa_stream_set_moved_callback(handle->stream, &AudioCaptureWorker::streamMovedCallback, handle.get());
    pa_stream_set_suspended_callback(handle->stream, &AudioCaptureWorker::streamSuspendedCallback, handle.get());

    if (pa_stream_connect_record(handle->stream, sourceName.constData(), &bufferAttr, PA_STREAM_ADJUST_LATENCY) < 0) {
        *errorMessage = pulseError(m_context, u"Could not connect a PulseAudio record stream"_s);
        pa_stream_unref(handle->stream);
        return false;
    }
    if (!waitForStreamReady(handle->stream, errorMessage)) {
        pa_stream_disconnect(handle->stream);
        pa_stream_unref(handle->stream);
        return false;
    }

    m_streams.push_back(std::move(handle));
    return true;
}

bool AudioCaptureWorker::validateSourceExists(const QByteArray& sourceName, QString* errorMessage)
{
    SourceInfoResult sourceInfo;
    sourceInfo.mainloop = m_mainloop;
    auto operation = pa_context_get_source_info_by_name(m_context, sourceName.constData(), &sourceInfoCallback, &sourceInfo);
    if (!operation) {
        *errorMessage = pulseError(m_context, u"Could not query a PulseAudio source"_s);
        return false;
    }
    while (!sourceInfo.done) {
        pa_threaded_mainloop_wait(m_mainloop);
    }
    pa_operation_unref(operation);
    if (sourceInfo.failed || !sourceInfo.exists) {
        *errorMessage = u"PulseAudio source is not available: "_s + QString::fromUtf8(sourceName);
        return false;
    }
    return true;
}

void AudioCaptureWorker::cleanupPulse()
{
    if (!m_mainloop) {
        return;
    }

    pa_threaded_mainloop_lock(m_mainloop);
    for (auto& handle : m_streams) {
        if (!handle || !handle->stream) {
            continue;
        }
        handle->active = false;
        pa_stream_set_read_callback(handle->stream, nullptr, nullptr);
        pa_stream_set_state_callback(handle->stream, nullptr, nullptr);
        pa_stream_set_moved_callback(handle->stream, nullptr, nullptr);
        pa_stream_set_suspended_callback(handle->stream, nullptr, nullptr);
        pa_stream_disconnect(handle->stream);
        pa_stream_unref(handle->stream);
        handle->stream = nullptr;
    }
    m_streams.clear();

    if (m_context) {
        pa_context_set_state_callback(m_context, nullptr, nullptr);
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_stop(m_mainloop);
    pa_threaded_mainloop_free(m_mainloop);
    m_mainloop = nullptr;
}

void AudioCaptureWorker::handleStreamReadable(StreamHandle* handle)
{
    if (!handle || !handle->active || !m_callbackActive.load(std::memory_order_acquire)) {
        return;
    }

    while (handle->active && m_callbackActive.load(std::memory_order_acquire)) {
        const void* data = nullptr;
        size_t nbytes = 0;
        if (pa_stream_peek(handle->stream, &data, &nbytes) < 0) {
            postFailure(u"PulseAudio stream read failed: "_s + QString::fromUtf8(pa_strerror(pa_context_errno(m_context))));
            return;
        }
        if (nbytes == 0) {
            return;
        }

        AudioPcmMixer::PushResult result = AudioPcmMixer::PushResult::InvalidData;
        if (data) {
            result = m_mixer.pushSourceData(handle->mixerIndex, QByteArray(static_cast<const char*>(data), qsizetype(nbytes)));
        } else if (nbytes % AudioPcmMixer::BytesPerFrame == 0) {
            result = m_mixer.pushSilenceFrames(handle->mixerIndex, qint64(nbytes / AudioPcmMixer::BytesPerFrame));
        }

        pa_stream_drop(handle->stream);

        if (result == AudioPcmMixer::PushResult::QueueOverflow) {
            postFailure(u"PulseAudio audio input queue exceeded two seconds"_s);
            return;
        }
        if (result != AudioPcmMixer::PushResult::Accepted) {
            postFailure(u"PulseAudio delivered invalid PCM data"_s);
            return;
        }

        drainMixerToOutputQueue();
    }
}

void AudioCaptureWorker::handleStreamStateChanged(StreamHandle* handle)
{
    if (m_mainloop) {
        pa_threaded_mainloop_signal(m_mainloop, 0);
    }
    if (!handle || !handle->active || !m_running.load(std::memory_order_acquire)) {
        return;
    }
    const auto state = pa_stream_get_state(handle->stream);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        postFailure(u"PulseAudio record stream stopped unexpectedly for source "_s + QString::fromUtf8(handle->sourceName));
    }
}

void AudioCaptureWorker::handleStreamMoved(StreamHandle* handle)
{
    if (handle && handle->active && m_running.load(std::memory_order_acquire)) {
        postFailure(u"PulseAudio record stream moved away from requested source "_s + QString::fromUtf8(handle->sourceName));
    }
}

void AudioCaptureWorker::handleStreamSuspended(StreamHandle* handle)
{
    if (handle && handle->active && m_running.load(std::memory_order_acquire)) {
        postFailure(u"PulseAudio record stream was suspended for source "_s + QString::fromUtf8(handle->sourceName));
    }
}

void AudioCaptureWorker::drainMixerToOutputQueue()
{
    bool queuedAny = false;
    TimestampedPcmChunk chunk;
    while (m_mixer.popMixedChunk(&chunk, m_chunkFrames)) {
        {
            QMutexLocker locker(&m_outputMutex);
            if (m_outputQueuedFrames + chunk.frameCount > m_maxQueuedFrames) {
                postFailure(u"Mixed audio output queue exceeded two seconds"_s);
                return;
            }
            m_outputQueuedFrames += chunk.frameCount;
            m_outputQueue.enqueue(chunk);
            queuedAny = true;
        }
    }

    if (queuedAny) {
        QMetaObject::invokeMethod(this, [this] {
            if (m_running.load(std::memory_order_acquire)) {
                Q_EMIT chunkReady();
            } }, Qt::QueuedConnection);
    }
}

void AudioCaptureWorker::postFailure(const QString& message)
{
    if (m_failurePosted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_callbackActive.store(false, std::memory_order_release);
    QMetaObject::invokeMethod(this, [this, message] {
        if (m_running.load(std::memory_order_acquire)) {
            Q_EMIT failed(message);
        }
        stopCapture(); }, Qt::QueuedConnection);
}

void AudioCaptureWorker::emitFailureNow(const QString& message)
{
    if (!m_failurePosted.exchange(true, std::memory_order_acq_rel)) {
        Q_EMIT failed(message);
    }
}

void AudioCaptureWorker::contextStateCallback(pa_context* context, void* userdata)
{
    auto worker = static_cast<AudioCaptureWorker*>(userdata);
    if (worker->m_mainloop) {
        pa_threaded_mainloop_signal(worker->m_mainloop, 0);
    }
    const auto state = pa_context_get_state(context);
    if (worker->m_running.load(std::memory_order_acquire) && (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)) {
        worker->postFailure(pulseError(context, u"PulseAudio context stopped unexpectedly"_s));
    }
}

void AudioCaptureWorker::streamReadCallback(pa_stream*, size_t, void* userdata)
{
    auto handle = static_cast<StreamHandle*>(userdata);
    if (handle && handle->worker) {
        handle->worker->handleStreamReadable(handle);
    }
}

void AudioCaptureWorker::streamStateCallback(pa_stream*, void* userdata)
{
    auto handle = static_cast<StreamHandle*>(userdata);
    if (handle && handle->worker) {
        handle->worker->handleStreamStateChanged(handle);
    }
}

void AudioCaptureWorker::streamMovedCallback(pa_stream*, void* userdata)
{
    auto handle = static_cast<StreamHandle*>(userdata);
    if (handle && handle->worker) {
        handle->worker->handleStreamMoved(handle);
    }
}

void AudioCaptureWorker::streamSuspendedCallback(pa_stream*, void* userdata)
{
    auto handle = static_cast<StreamHandle*>(userdata);
    if (handle && handle->worker) {
        handle->worker->handleStreamSuspended(handle);
    }
}

} // namespace Spectacle::Audio

#include "moc_AudioCaptureWorker.cpp"
