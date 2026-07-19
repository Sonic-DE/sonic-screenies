/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "Platforms/Encoding/AnimatedEncoderWorker.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

QImage syntheticFrame(const QSize& size, int index)
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            line[x] = qRgb((x * 7 + index * 31) % 256, (y * 11 + index * 17) % 256, (x + y + index * 23) % 256);
        }
    }
    return image;
}

QString extensionFor(Spectacle::Encoding::AnimatedEncoderWorker::Format format)
{
    return format == Spectacle::Encoding::AnimatedEncoderWorker::Format::WebP ? u"webp"_s : u"gif"_s;
}

} // namespace

class AnimatedEncoderWorkerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void probesAreCallable();
    void encodesSyntheticFrames_data();
    void encodesSyntheticFrames();
    void failureRemovesIncompleteFile();
    void cancelRemovesIncompleteFile();
};

void AnimatedEncoderWorkerTest::probesAreCallable()
{
    const bool gifSupported = Spectacle::Encoding::AnimatedEncoderWorker::isGifSupported();
    QCOMPARE(gifSupported, Spectacle::Encoding::AnimatedEncoderWorker::isFormatSupported(Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif));
    if (!gifSupported) {
        QVERIFY(!Spectacle::Encoding::AnimatedEncoderWorker::unavailableReason(Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif).isEmpty());
    }

    const bool webpSupported = Spectacle::Encoding::AnimatedEncoderWorker::isAnimatedWebPSupported();
    QCOMPARE(webpSupported, Spectacle::Encoding::AnimatedEncoderWorker::isFormatSupported(Spectacle::Encoding::AnimatedEncoderWorker::Format::WebP));
    if (!webpSupported) {
        QVERIFY(!Spectacle::Encoding::AnimatedEncoderWorker::unavailableReason(Spectacle::Encoding::AnimatedEncoderWorker::Format::WebP).isEmpty());
    }
}

void AnimatedEncoderWorkerTest::encodesSyntheticFrames_data()
{
    QTest::addColumn<Spectacle::Encoding::AnimatedEncoderWorker::Format>("format");
    QTest::newRow("gif") << Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif;
    QTest::newRow("webp") << Spectacle::Encoding::AnimatedEncoderWorker::Format::WebP;
}

void AnimatedEncoderWorkerTest::encodesSyntheticFrames()
{
    QFETCH(Spectacle::Encoding::AnimatedEncoderWorker::Format, format);
    if (!Spectacle::Encoding::AnimatedEncoderWorker::isFormatSupported(format)) {
        QSKIP(qPrintable(Spectacle::Encoding::AnimatedEncoderWorker::unavailableReason(format)));
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QSize size(96, 64);
    const auto output = QUrl::fromLocalFile(directory.filePath(u"animation."_s + extensionFor(format)));

    Spectacle::Encoding::AnimatedEncoderWorker worker;
    QSignalSpy finishedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::finished);
    QSignalSpy failedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::failed);
    QSignalSpy canceledSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::canceled);
    QVERIFY(finishedSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(canceledSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(canceledSpy.isValid());

    Spectacle::Encoding::AnimatedEncoderWorker::Options options;
    options.format = format;
    options.outputUrl = output;
    options.frameSize = size;
    options.maximumQueuedFrames = 4;
    worker.start(options);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty() ? QString() : failedSpy.first().first().toString()));

    for (int i = 0; i < 6; ++i) {
        worker.submitFrame(syntheticFrame(size, i), i * 100000, (i + 1) * 100000);
    }
    worker.finish();

    QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty() || !failedSpy.isEmpty(), 10000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty() ? QString() : failedSpy.first().first().toString()));
    QCOMPARE(finishedSpy.size(), 1);
    QCOMPARE(canceledSpy.size(), 0);
    QCOMPARE(finishedSpy.first().first().toUrl(), output);
    const QFileInfo outputInfo(output.toLocalFile());
    QVERIFY(outputInfo.exists());
    QVERIFY2(outputInfo.size() > 0, "The animated encoder created an empty output file");
}

void AnimatedEncoderWorkerTest::failureRemovesIncompleteFile()
{
    if (!Spectacle::Encoding::AnimatedEncoderWorker::isGifSupported()) {
        QSKIP(qPrintable(Spectacle::Encoding::AnimatedEncoderWorker::unavailableReason(Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif)));
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QSize size(64, 64);
    const auto output = QUrl::fromLocalFile(directory.filePath(u"failure.gif"_s));

    Spectacle::Encoding::AnimatedEncoderWorker worker;
    QSignalSpy finishedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::finished);
    QSignalSpy failedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::failed);
    QSignalSpy canceledSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::canceled);
    QVERIFY(finishedSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(canceledSpy.isValid());

    Spectacle::Encoding::AnimatedEncoderWorker::Options options;
    options.format = Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif;
    options.outputUrl = output;
    options.frameSize = size;
    worker.start(options);
    worker.submitFrame(syntheticFrame(size, 0), 0, 100000);
    worker.submitFrame(syntheticFrame(QSize(32, 32), 1), 100000, 200000);

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.size(), 1, 5000);
    QCOMPARE(finishedSpy.size(), 0);
    QCOMPARE(canceledSpy.size(), 0);
    QVERIFY(!QFileInfo::exists(output.toLocalFile()));
}

void AnimatedEncoderWorkerTest::cancelRemovesIncompleteFile()
{
    if (!Spectacle::Encoding::AnimatedEncoderWorker::isGifSupported()) {
        QSKIP(qPrintable(Spectacle::Encoding::AnimatedEncoderWorker::unavailableReason(Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif)));
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QSize size(64, 64);
    const auto output = QUrl::fromLocalFile(directory.filePath(u"canceled.gif"_s));

    Spectacle::Encoding::AnimatedEncoderWorker worker;
    QSignalSpy finishedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::finished);
    QSignalSpy failedSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::failed);
    QSignalSpy canceledSpy(&worker, &Spectacle::Encoding::AnimatedEncoderWorker::canceled);

    Spectacle::Encoding::AnimatedEncoderWorker::Options options;
    options.format = Spectacle::Encoding::AnimatedEncoderWorker::Format::Gif;
    options.outputUrl = output;
    options.frameSize = size;
    worker.start(options);
    worker.submitFrame(syntheticFrame(size, 0), 0, 100000);
    worker.cancel();

    QCOMPARE(canceledSpy.size(), 1);
    QCOMPARE(finishedSpy.size(), 0);
    QCOMPARE(failedSpy.size(), 0);
    QVERIFY(!QFileInfo::exists(output.toLocalFile()));
}

QTEST_MAIN(AnimatedEncoderWorkerTest)
#include "AnimatedEncoderWorkerTest.moc"
