/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "AudioPcmMixer.h"

#include <QtGlobal>

#include <algorithm>
#include <cstring>

namespace Spectacle::Audio {

AudioPcmMixer::AudioPcmMixer(int sourceCount, qint64 maxQueuedFramesPerSource)
{
    reset(sourceCount, maxQueuedFramesPerSource);
}

void AudioPcmMixer::reset(int sourceCount, qint64 maxQueuedFramesPerSource)
{
    QMutexLocker locker(&m_mutex);
    m_sources = QVector<QByteArray>(std::max(0, sourceCount));
    m_maxQueuedFramesPerSource = std::max<qint64>(0, maxQueuedFramesPerSource);
    m_totalFramesMixed = 0;
}

int AudioPcmMixer::sourceCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_sources.size();
}

qint64 AudioPcmMixer::totalFramesMixed() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalFramesMixed;
}

qint64 AudioPcmMixer::queuedFrames(int sourceIndex) const
{
    QMutexLocker locker(&m_mutex);
    return isValidSourceLocked(sourceIndex) ? queuedFramesLocked(sourceIndex) : 0;
}

AudioPcmMixer::PushResult AudioPcmMixer::pushSourceData(int sourceIndex, const QByteArray& pcm)
{
    if (pcm.isEmpty()) {
        return PushResult::Accepted;
    }
    if (pcm.size() % BytesPerFrame != 0) {
        return PushResult::InvalidData;
    }

    QMutexLocker locker(&m_mutex);
    if (!isValidSourceLocked(sourceIndex)) {
        return PushResult::InvalidSource;
    }

    const qint64 incomingFrames = pcm.size() / BytesPerFrame;
    if (queuedFramesLocked(sourceIndex) + incomingFrames > m_maxQueuedFramesPerSource) {
        return PushResult::QueueOverflow;
    }

    m_sources[sourceIndex].append(pcm);
    return PushResult::Accepted;
}

AudioPcmMixer::PushResult AudioPcmMixer::pushSilenceFrames(int sourceIndex, qint64 frameCount)
{
    if (frameCount < 0) {
        return PushResult::InvalidData;
    }
    return pushSourceData(sourceIndex, silenceFrames(frameCount));
}

bool AudioPcmMixer::canPopMixedChunk(qint64 maxFrames) const
{
    QMutexLocker locker(&m_mutex);
    if (m_sources.isEmpty() || maxFrames <= 0) {
        return false;
    }
    return std::all_of(m_sources.cbegin(), m_sources.cend(), [](const QByteArray& queue) {
        return queue.size() >= BytesPerFrame;
    });
}

bool AudioPcmMixer::popMixedChunk(TimestampedPcmChunk* chunk, qint64 maxFrames)
{
    if (!chunk) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    if (m_sources.isEmpty() || maxFrames <= 0) {
        return false;
    }

    qint64 framesToMix = maxFrames;
    for (const auto& queue : std::as_const(m_sources)) {
        framesToMix = std::min<qint64>(framesToMix, queue.size() / BytesPerFrame);
    }
    if (framesToMix <= 0) {
        return false;
    }

    QByteArray mixed;
    mixed.resize(qsizetype(framesToMix * BytesPerFrame));
    auto output = reinterpret_cast<float*>(mixed.data());
    const qint64 sampleCount = framesToMix * Channels;

    for (qint64 sample = 0; sample < sampleCount; ++sample) {
        float value = 0.0f;
        for (const auto& queue : std::as_const(m_sources)) {
            const auto input = reinterpret_cast<const float*>(queue.constData());
            value += input[sample];
        }
        output[sample] = std::clamp(value, -1.0f, 1.0f);
    }

    const qsizetype bytesToRemove = qsizetype(framesToMix * BytesPerFrame);
    for (auto& queue : m_sources) {
        queue.remove(0, bytesToRemove);
    }

    chunk->pcm = std::move(mixed);
    chunk->startFrame = m_totalFramesMixed;
    chunk->frameCount = framesToMix;
    chunk->startTimeUs = framesToUsec(m_totalFramesMixed);
    m_totalFramesMixed += framesToMix;
    chunk->endTimeUs = framesToUsec(m_totalFramesMixed);
    return true;
}

qint64 AudioPcmMixer::framesToUsec(qint64 frames)
{
    return (frames * 1000000) / SampleRate;
}

QByteArray AudioPcmMixer::silenceFrames(qint64 frameCount)
{
    if (frameCount <= 0) {
        return {};
    }
    return QByteArray(qsizetype(frameCount * BytesPerFrame), '\0');
}

bool AudioPcmMixer::isValidSourceLocked(int sourceIndex) const
{
    return sourceIndex >= 0 && sourceIndex < m_sources.size();
}

qint64 AudioPcmMixer::queuedFramesLocked(int sourceIndex) const
{
    return m_sources[sourceIndex].size() / BytesPerFrame;
}

} // namespace Spectacle::Audio
