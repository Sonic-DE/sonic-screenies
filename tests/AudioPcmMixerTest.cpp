/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/Audio/AudioPcmMixer.h"

#include <QtTest>

using Spectacle::Audio::AudioPcmMixer;
using Spectacle::Audio::TimestampedPcmChunk;

namespace {
QByteArray pcm(std::initializer_list<float> samples)
{
    QByteArray data;
    data.resize(qsizetype(samples.size() * sizeof(float)));
    auto output = reinterpret_cast<float*>(data.data());
    int index = 0;
    for (float sample : samples) {
        output[index++] = sample;
    }
    return data;
}

QVector<float> samples(const QByteArray& data)
{
    QVector<float> values;
    const auto input = reinterpret_cast<const float*>(data.constData());
    for (qsizetype i = 0; i < data.size() / qsizetype(sizeof(float)); ++i) {
        values.push_back(input[i]);
    }
    return values;
}
} // namespace

class AudioPcmMixerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void clipsMixedSamples();
    void mixesUnequalChunksInOrder();
    void resetClearsQueuesAndTimestamps();
    void timestampsComeFromTotalFrames();
    void silenceHolesMixAsZeroes();
};

void AudioPcmMixerTest::clipsMixedSamples()
{
    AudioPcmMixer mixer(2);
    QCOMPARE(mixer.pushSourceData(0, pcm({0.75f, -0.75f})), AudioPcmMixer::PushResult::Accepted);
    QCOMPARE(mixer.pushSourceData(1, pcm({0.75f, -0.75f})), AudioPcmMixer::PushResult::Accepted);

    TimestampedPcmChunk chunk;
    QVERIFY(mixer.popMixedChunk(&chunk, 1));
    QCOMPARE(samples(chunk.pcm), QVector<float>({1.0f, -1.0f}));
}

void AudioPcmMixerTest::mixesUnequalChunksInOrder()
{
    AudioPcmMixer mixer(2);
    QCOMPARE(mixer.pushSourceData(0, pcm({0.1f, 0.2f, 0.3f, 0.4f})), AudioPcmMixer::PushResult::Accepted);
    QCOMPARE(mixer.pushSourceData(1, pcm({0.5f, 0.6f})), AudioPcmMixer::PushResult::Accepted);

    TimestampedPcmChunk chunk;
    QVERIFY(mixer.popMixedChunk(&chunk, 8));
    QCOMPARE(chunk.frameCount, 1);
    QCOMPARE(samples(chunk.pcm), QVector<float>({0.6f, 0.8f}));
    QCOMPARE(mixer.queuedFrames(0), 1);
    QCOMPARE(mixer.queuedFrames(1), 0);

    QCOMPARE(mixer.pushSourceData(1, pcm({0.7f, 0.8f})), AudioPcmMixer::PushResult::Accepted);
    QVERIFY(mixer.popMixedChunk(&chunk, 8));
    QCOMPARE(samples(chunk.pcm), QVector<float>({1.0f, 1.0f}));
}

void AudioPcmMixerTest::resetClearsQueuesAndTimestamps()
{
    AudioPcmMixer mixer(1);
    QCOMPARE(mixer.pushSourceData(0, pcm({0.1f, 0.2f})), AudioPcmMixer::PushResult::Accepted);
    TimestampedPcmChunk chunk;
    QVERIFY(mixer.popMixedChunk(&chunk));
    QCOMPARE(mixer.totalFramesMixed(), 1);

    mixer.reset(1);
    QCOMPARE(mixer.totalFramesMixed(), 0);
    QCOMPARE(mixer.queuedFrames(0), 0);
    QVERIFY(!mixer.popMixedChunk(&chunk));

    QCOMPARE(mixer.pushSourceData(0, pcm({0.3f, 0.4f})), AudioPcmMixer::PushResult::Accepted);
    QVERIFY(mixer.popMixedChunk(&chunk));
    QCOMPARE(chunk.startFrame, 0);
    QCOMPARE(chunk.startTimeUs, 0);
}

void AudioPcmMixerTest::timestampsComeFromTotalFrames()
{
    AudioPcmMixer mixer(1);
    QCOMPARE(mixer.pushSilenceFrames(0, 24000), AudioPcmMixer::PushResult::Accepted);

    TimestampedPcmChunk chunk;
    QCOMPARE(mixer.pushSilenceFrames(0, 24000), AudioPcmMixer::PushResult::Accepted);
    QVERIFY(mixer.popMixedChunk(&chunk, 24000));
    QCOMPARE(chunk.startFrame, 0);
    QCOMPARE(chunk.frameCount, 24000);
    QCOMPARE(chunk.startTimeUs, 0);
    QCOMPARE(chunk.endTimeUs, 500000);

    QVERIFY(mixer.popMixedChunk(&chunk, 24000));
    QCOMPARE(chunk.startFrame, 24000);
    QCOMPARE(chunk.startTimeUs, 500000);
    QCOMPARE(chunk.endTimeUs, 1000000);
}

void AudioPcmMixerTest::silenceHolesMixAsZeroes()
{
    AudioPcmMixer mixer(2);
    QCOMPARE(mixer.pushSourceData(0, pcm({0.25f, -0.25f})), AudioPcmMixer::PushResult::Accepted);
    QCOMPARE(mixer.pushSilenceFrames(1, 1), AudioPcmMixer::PushResult::Accepted);

    TimestampedPcmChunk chunk;
    QVERIFY(mixer.popMixedChunk(&chunk));
    QCOMPARE(samples(chunk.pcm), QVector<float>({0.25f, -0.25f}));
}

QTEST_GUILESS_MAIN(AudioPcmMixerTest)
#include "AudioPcmMixerTest.moc"
