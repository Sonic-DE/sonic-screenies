/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/VideoPlatform.h"
#include "Platforms/VideoPlatformX11.h"
#include "Platforms/X11/X11Connection.h"
#include "Platforms/X11/X11FrameGrabWorker.h"

#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {
QRect nativeScreenRect()
{
    X11::Connection connection;
    if (!connection.isValid()) {
        return {};
    }
    X11::ReplyPtr geometry(xcb_get_geometry_reply(connection.handle(), xcb_get_geometry(connection.handle(), connection.rootWindow()), nullptr));
    const auto root = geometry.get<xcb_get_geometry_reply_t>();
    return root ? QRect(0, 0, root->width, root->height) : QRect();
}

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
    void intentionalCaptureErrorMayDeleteWorker_data();
    void intentionalCaptureErrorMayDeleteWorker();
    void intentionalCaptureError_data();
    void intentionalCaptureError();
};

void VideoRecordingTest::formatHelpers()
{
    QCOMPARE(VideoPlatform::extensionForFormat(VideoPlatform::WebM_VP9), u"webm"_s);
    QCOMPARE(VideoPlatform::extensionForFormat(VideoPlatform::MP4_H264), u"mp4"_s);
    QCOMPARE(VideoPlatform::formatForExtension(u"WEBM"_s), VideoPlatform::WebM_VP9);
    QVERIFY(VideoPlatform::formatSupportsAudio(VideoPlatform::WebM_VP9));
    QVERIFY(!VideoPlatform::formatSupportsAudio(VideoPlatform::Gif));
}

void VideoRecordingTest::intentionalCaptureErrorMayDeleteWorker_data()
{
    QTest::addColumn<bool>("queryGeometry");
    QTest::newRow("geometry-error") << true;
    QTest::newRow("get-image-error") << false;
}

void VideoRecordingTest::intentionalCaptureErrorMayDeleteWorker()
{
    if (QGuiApplication::platformName() != u"xcb"_s) {
        QSKIP("Requires a private X11 test display");
    }
    QFETCH(bool, queryGeometry);
    X11::Connection connection;
    QVERIFY(connection.isValid());
    QString failure;
    QObject owner;
    auto* worker = new X11::FrameGrabWorker(&owner);
    QPointer<X11::FrameGrabWorker> alive(worker);
    QSignalSpy stopped(worker, &X11::FrameGrabWorker::producerStopped);
    connect(worker, &X11::FrameGrabWorker::producerFailed, &owner, [&](const QString& message) {
        failure = message;
        delete worker; // Match the controller's synchronous failure cleanup.
    });
    X11::FrameGrabOptions options;
    options.source = X11::FrameGrabOptions::Source::Window;
    options.windowId = xcb_generate_id(connection.handle()); // Deliberately never create this window.
    if (!queryGeometry) {
        options.targetRect = QRect(0, 0, 64, 64);
    }
    worker->setOptions(options);
    worker->startWorker();
    worker->startCapture();
    QTRY_VERIFY_WITH_TIMEOUT(alive.isNull(), 3000);
    QVERIFY(!failure.isEmpty());
    QCOMPARE(stopped.size(), 1);
}

void VideoRecordingTest::intentionalCaptureError_data()
{
    QTest::addColumn<bool>("animated");
    QTest::addColumn<bool>("badDisplay");
    QTest::newRow("qt-invalid-rectangle") << false << false;
    QTest::newRow("qt-display-connection-failure") << false << true;
    QTest::newRow("gif-invalid-rectangle") << true << false;
    QTest::newRow("gif-display-connection-failure") << true << true;
}

void VideoRecordingTest::intentionalCaptureError()
{
    if (QGuiApplication::platformName() != u"xcb"_s) {
        QSKIP("Requires a private X11 test display");
    }
    QFETCH(bool, animated);
    QFETCH(bool, badDisplay);
    Spectacle::VideoPlatformX11 platform;
    const auto formats = platform.supportedFormats();
    const auto qtFormat = formats.testFlag(VideoPlatform::MP4_H264) ? VideoPlatform::MP4_H264 : VideoPlatform::WebM_VP9;
    const auto format = animated ? VideoPlatform::Gif : qtFormat;
    if (!formats.testFlag(format)) {
        QSKIP("The required video encoder is unavailable");
    }
    const auto root = nativeScreenRect();
    QVERIFY(!root.isEmpty());
    const QRect rect(badDisplay ? 0 : root.width() - 32, 0, 64, 64);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto output = QUrl::fromLocalFile(directory.filePath(u"failure."_s + VideoPlatform::extensionForFormat(format)));
    QSignalSpy failed(&platform, &VideoPlatform::recordingFailed);
    QSignalSpy saved(&platform, &VideoPlatform::recordingSaved);
    QSignalSpy canceled(&platform, &VideoPlatform::recordingCanceled);
    const auto display = qgetenv("DISPLAY");
    if (badDisplay) {
        qputenv("DISPLAY", "not-a-display");
    }
    platform.startRecording(output, VideoPlatform::Region, {{u"rect"_s, rect}}, false);
    if (badDisplay) {
        qputenv("DISPLAY", display);
    }
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
    QVERIFY(failed.first().first().toString().contains(badDisplay ? u"Could not connect to the X11 display"_s : u"X11 returned an empty video frame"_s));
    QVERIFY(!platform.isBusy());
    QCOMPARE(saved.size(), 0);
    QCOMPARE(canceled.size(), 0);
    QVERIFY(!QFileInfo::exists(output.toLocalFile()));
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

    // Explicit capture rectangles are native X11 coordinates, not Qt's
    // potentially different logical screen geometry.
    const auto screen = nativeScreenRect();
    QVERIFY(!screen.isEmpty());
    platform.startRecording(output, VideoPlatform::Screen, {{u"rect"_s, screen}}, false);
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
