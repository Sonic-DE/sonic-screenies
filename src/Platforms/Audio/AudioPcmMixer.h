/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QByteArray>
#include <QMutex>
#include <QVector>

namespace Spectacle::Audio {

struct TimestampedPcmChunk {
    QByteArray pcm;
    qint64 startTimeUs = 0;
    qint64 endTimeUs = 0;
    qint64 startFrame = 0;
    qint64 frameCount = 0;

    [[nodiscard]] bool isValid() const
    {
        return frameCount > 0 && !pcm.isEmpty() && endTimeUs >= startTimeUs;
    }
};

class AudioPcmMixer {
public:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;
    static constexpr int BytesPerSample = int(sizeof(float));
    static constexpr int BytesPerFrame = Channels * BytesPerSample;
    static constexpr qint64 DefaultMaxQueuedFrames = SampleRate * 2;
    static constexpr qint64 DefaultChunkFrames = 1024;

    enum class PushResult {
        Accepted,
        InvalidSource,
        InvalidData,
        QueueOverflow,
    };

    explicit AudioPcmMixer(int sourceCount = 0, qint64 maxQueuedFramesPerSource = DefaultMaxQueuedFrames);

    void reset(int sourceCount = 0, qint64 maxQueuedFramesPerSource = DefaultMaxQueuedFrames);
    [[nodiscard]] int sourceCount() const;
    [[nodiscard]] qint64 totalFramesMixed() const;
    [[nodiscard]] qint64 queuedFrames(int sourceIndex) const;

    PushResult pushSourceData(int sourceIndex, const QByteArray& pcm);
    PushResult pushSilenceFrames(int sourceIndex, qint64 frameCount);

    [[nodiscard]] bool canPopMixedChunk(qint64 maxFrames = DefaultChunkFrames) const;
    [[nodiscard]] bool popMixedChunk(TimestampedPcmChunk* chunk, qint64 maxFrames = DefaultChunkFrames);

    static qint64 framesToUsec(qint64 frames);
    static QByteArray silenceFrames(qint64 frameCount);

private:
    [[nodiscard]] bool isValidSourceLocked(int sourceIndex) const;
    [[nodiscard]] qint64 queuedFramesLocked(int sourceIndex) const;

    mutable QMutex m_mutex;
    QVector<QByteArray> m_sources;
    qint64 m_maxQueuedFramesPerSource = DefaultMaxQueuedFrames;
    qint64 m_totalFramesMixed = 0;
};

} // namespace Spectacle::Audio
