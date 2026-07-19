/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/VideoPlatform.h"
#include "Platforms/VideoPlatformX11.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {
void verifyEncodedOutput(const QUrl& output, const QByteArray& expectedCodec, const QByteArray& expectedContainer)
{
    QVERIFY(output.isLocalFile());
    const QFileInfo outputInfo(output.toLocalFile());
    QVERIFY(outputInfo.exists());
    QVERIFY2(outputInfo.size() > 0, "The recorder created an empty output file");

    const auto ffprobe = QStandardPaths::findExecutable(u"ffprobe"_s);
    if (ffprobe.isEmpty()) {
        return;
    }

    QProcess probe;
    probe.start(ffprobe,
        {u"-v"_s,
            u"error"_s,
            u"-select_streams"_s,
            u"v:0"_s,
            u"-show_entries"_s,
            u"stream=codec_name,width,height:format=format_name,duration"_s,
            u"-of"_s,
            u"default=noprint_wrappers=1"_s,
            output.toLocalFile()});
    QVERIFY2(probe.waitForFinished(10000), qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
    const auto metadata = probe.readAllStandardOutput();
    QByteArray codecEntry("codec_name=");
    codecEntry.append(expectedCodec);
    QVERIFY2(metadata.contains(codecEntry), metadata.constData());
    QVERIFY2(metadata.contains("format_name=") && metadata.contains(expectedContainer), metadata.constData());
    QVERIFY2(metadata.contains("width=") && metadata.contains("height="), metadata.constData());

    const QRegularExpression durationExpression(u"(?:^|\\n)duration=([0-9]+(?:\\.[0-9]+)?)"_s);
    const auto durationMatch = durationExpression.match(QString::fromUtf8(metadata));
    QVERIFY2(durationMatch.hasMatch(), metadata.constData());
    bool durationOk = false;
    const double duration = durationMatch.capturedView(1).toDouble(&durationOk);
    QVERIFY(durationOk);
    QVERIFY2(duration > 0.0, metadata.constData());
}
}

class VideoRecordingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void formatHelpers();
    void recordsX11ScreenToWebM();
    void recordsSelectedRegion();
    void recordsAnimatedGif();
};

void VideoRecordingTest::formatHelpers()
{
    QCOMPARE(VideoPlatform::extensionForFormat(VideoPlatform::WebM_VP9), u"webm"_s);
    QCOMPARE(VideoPlatform::extensionForFormat(VideoPlatform::MP4_H264), u"mp4"_s);
    QCOMPARE(VideoPlatform::formatForExtension(u"WEBM"_s), VideoPlatform::WebM_VP9);
    QVERIFY(VideoPlatform::formatSupportsAudio(VideoPlatform::WebM_VP9));
    QVERIFY(!VideoPlatform::formatSupportsAudio(VideoPlatform::Gif));
}

void VideoRecordingTest::recordsX11ScreenToWebM()
{
    if (QGuiApplication::platformName() != u"xcb"_s) {
        QSKIP("This integration test requires the Qt xcb platform");
    }

    Spectacle::VideoPlatformX11 platform;
    const auto formats = platform.supportedFormats();
    const auto format = formats.testFlag(VideoPlatform::WebM_VP9)
        ? VideoPlatform::WebM_VP9
        : (formats.testFlag(VideoPlatform::MP4_H264) ? VideoPlatform::MP4_H264 : VideoPlatform::NoFormat);
    if (format == VideoPlatform::NoFormat) {
        QSKIP("The installed Qt Multimedia backend has no supported video encoder");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QUrl output = QUrl::fromLocalFile(directory.filePath(u"recording."_s + VideoPlatform::extensionForFormat(format)));

    QSignalSpy stateSpy(&platform, &VideoPlatform::recordingStateChanged);
    QSignalSpy savedSpy(&platform, &VideoPlatform::recordingSaved);
    QSignalSpy failedSpy(&platform, &VideoPlatform::recordingFailed);
    QSignalSpy canceledSpy(&platform, &VideoPlatform::recordingCanceled);
    QVERIFY(stateSpy.isValid());
    QVERIFY(savedSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(canceledSpy.isValid());

    const auto screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    platform.startRecording(output, VideoPlatform::Screen, {{u"rect"_s, screen->geometry()}}, false);
    QTRY_VERIFY_WITH_TIMEOUT(platform.isRecording() || !failedSpy.isEmpty(), 10000);
    if (!failedSpy.isEmpty()) {
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    }

    QTest::qWait(750);
    platform.finishRecording();
    QTRY_VERIFY_WITH_TIMEOUT(!savedSpy.isEmpty() || !failedSpy.isEmpty(), 15000);
    if (!failedSpy.isEmpty()) {
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    }

    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(failedSpy.size(), 0);
    QCOMPARE(canceledSpy.size(), 0);
    QVERIFY(!platform.isBusy());
    const auto savedUrl = savedSpy.first().first().toUrl();
    QCOMPARE(savedUrl.toLocalFile(), output.toLocalFile());
    verifyEncodedOutput(output,
        format == VideoPlatform::WebM_VP9 ? QByteArray("vp9") : QByteArray("h264"),
        format == VideoPlatform::WebM_VP9 ? QByteArray("webm") : QByteArray("mp4"));
}

void VideoRecordingTest::recordsSelectedRegion()
{
    if (QGuiApplication::platformName() != u"xcb"_s) {
        QSKIP("This integration test requires the Qt xcb platform");
    }

    Spectacle::VideoPlatformX11 platform;
    const auto formats = platform.supportedFormats();
    const auto format = formats.testFlag(VideoPlatform::WebM_VP9)
        ? VideoPlatform::WebM_VP9
        : (formats.testFlag(VideoPlatform::MP4_H264) ? VideoPlatform::MP4_H264 : VideoPlatform::NoFormat);
    if (format == VideoPlatform::NoFormat) {
        QSKIP("The installed Qt Multimedia backend has no supported video encoder");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QUrl output = QUrl::fromLocalFile(directory.filePath(u"region."_s + VideoPlatform::extensionForFormat(format)));
    QSignalSpy regionSpy(&platform, &VideoPlatform::regionRequested);
    QSignalSpy savedSpy(&platform, &VideoPlatform::recordingSaved);
    QSignalSpy failedSpy(&platform, &VideoPlatform::recordingFailed);
    QSignalSpy canceledSpy(&platform, &VideoPlatform::recordingCanceled);

    platform.startRecording(output, VideoPlatform::Region, {}, false);
    QCOMPARE(regionSpy.size(), 1);
    QVERIFY(platform.isBusy());

    platform.startRecording(output, VideoPlatform::Region, {{u"rect"_s, QRect(0, 0, 320, 240)}}, false);
    QTRY_VERIFY_WITH_TIMEOUT(platform.isRecording() || !failedSpy.isEmpty(), 10000);
    if (!failedSpy.isEmpty()) {
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    }
    QTest::qWait(500);
    platform.finishRecording();
    QTRY_VERIFY_WITH_TIMEOUT(!savedSpy.isEmpty() || !failedSpy.isEmpty(), 15000);
    if (!failedSpy.isEmpty()) {
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    }
    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(failedSpy.size(), 0);
    QCOMPARE(canceledSpy.size(), 0);
    QVERIFY(!platform.isBusy());
    verifyEncodedOutput(output,
        format == VideoPlatform::WebM_VP9 ? QByteArray("vp9") : QByteArray("h264"),
        format == VideoPlatform::WebM_VP9 ? QByteArray("webm") : QByteArray("mp4"));
}

void VideoRecordingTest::recordsAnimatedGif()
{
    if (QGuiApplication::platformName() != u"xcb"_s) {
        QSKIP("This integration test requires the Qt xcb platform");
    }
    Spectacle::VideoPlatformX11 platform;
    if (!platform.supportedFormats().testFlag(VideoPlatform::Gif)) {
        QSKIP("The installed FFmpeg build has no GIF encoder");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QUrl output = QUrl::fromLocalFile(directory.filePath(u"region.gif"_s));
    QSignalSpy savedSpy(&platform, &VideoPlatform::recordingSaved);
    QSignalSpy failedSpy(&platform, &VideoPlatform::recordingFailed);
    QSignalSpy canceledSpy(&platform, &VideoPlatform::recordingCanceled);
    QVERIFY(savedSpy.isValid());
    QVERIFY(failedSpy.isValid());

    platform.startRecording(output, VideoPlatform::Region, {{u"rect"_s, QRect(0, 0, 160, 120)}}, false);
    QTRY_VERIFY_WITH_TIMEOUT(platform.isRecording() || !failedSpy.isEmpty(), 10000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty() ? QString() : failedSpy.first().first().toString()));
    QTest::qWait(350);
    platform.finishRecording();
    QTRY_VERIFY_WITH_TIMEOUT(!savedSpy.isEmpty() || !failedSpy.isEmpty(), 10000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty() ? QString() : failedSpy.first().first().toString()));
    QCOMPARE(savedSpy.size(), 1);
    QCOMPARE(failedSpy.size(), 0);
    QCOMPARE(canceledSpy.size(), 0);
    QVERIFY(!platform.isBusy());
    verifyEncodedOutput(output, QByteArray("gif"), QByteArray("gif"));
}

QTEST_MAIN(VideoRecordingTest)
#include "VideoRecordingTest.moc"
