/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/X11/KWinWindowCapture.h"
#include "Platforms/ImagePlatformXcb.h"
#include "spectacle_debug.h"

#include <QBuffer>
#include <QDBusContext>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QFutureWatcher>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <QtConcurrentRun>
#include <QtGui/qguiapplication_platform.h>
#include <QtTest>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

Q_LOGGING_CATEGORY(SPECTACLE_LOG, "screenies.compositor.capture.test")

class ScreenshotService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.ScreenShot2")
public:
    QString mode;
    QString handle;
    QVariantMap options;
    QImage expected;
    struct Writer {
        int fd = -1;
        QByteArray bytes;
        qsizetype offset = 0;
        ~Writer()
        {
            if (fd >= 0)
                ::close(fd);
        }
    };
    QList<std::shared_ptr<Writer>> held;

public Q_SLOTS:
    QVariantMap CaptureWindow(const QString& window, const QVariantMap& flags, QDBusUnixFileDescriptor pipe)
    {
        handle = window;
        options = flags;
        if (mode == QLatin1String("denied")) {
            sendErrorReply(QStringLiteral("org.kde.KWin.ScreenShot2.Error.NoAuthorized"), QStringLiteral("The process is not authorized to take a screenshot"));
            return {};
        }
        const bool straight = mode == QLatin1String("straight");
        expected = QImage(mode == QLatin1String("large") ? QSize(384, 256) : QSize(6, 4),
            straight ? QImage::Format_ARGB32 : QImage::Format_ARGB32_Premultiplied);
        expected.fill(straight ? QRgb(0x80ff0000) : QRgb(0x80800000));
        QVariantMap metadata = {{QStringLiteral("type"), QStringLiteral("raw")},
            {QStringLiteral("format"), quint32(expected.format())},
            {QStringLiteral("width"), quint32(expected.width())}, {QStringLiteral("height"), quint32(expected.height())},
            {QStringLiteral("stride"), quint32(expected.bytesPerLine())}, {QStringLiteral("scale"), 2.0}};
        if (mode == QLatin1String("bad-type"))
            metadata[QStringLiteral("type")] = QStringLiteral("png");
        if (mode == QLatin1String("bad-format"))
            metadata[QStringLiteral("format")] = quint32(QImage::Format_Indexed8);
        if (mode == QLatin1String("bad-stride"))
            metadata[QStringLiteral("stride")] = quint32(expected.bytesPerLine() + 4);
        if (mode == QLatin1String("huge"))
            metadata[QStringLiteral("width")] = quint64(1) << 40;
        if (mode == QLatin1String("zero-size"))
            metadata[QStringLiteral("height")] = quint32(0);
        if (mode == QLatin1String("bad-scale"))
            metadata[QStringLiteral("scale")] = 0.0;

        const auto writer = std::make_shared<Writer>();
        writer->fd = ::fcntl(pipe.fileDescriptor(), F_DUPFD_CLOEXEC, 0);
        held.append(writer);
        if (mode != QLatin1String("valid") && mode != QLatin1String("straight") && mode != QLatin1String("large") && mode != QLatin1String("truncated")) {
            return metadata; // Keep the FD open for timeout/invalid-metadata tests.
        }
        writer->bytes = QByteArray(reinterpret_cast<const char*>(expected.constBits()), expected.sizeInBytes());
        if (mode == QLatin1String("truncated")) {
            writer->bytes.truncate(5);
        }
        auto* timer = new QTimer(this);
        timer->setInterval(2);
        connect(timer, &QTimer::timeout, timer, [timer, writer]() {
            const auto count = ::write(writer->fd, writer->bytes.constData() + writer->offset,
                std::min<qsizetype>(8192, writer->bytes.size() - writer->offset));
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            if (count > 0)
                writer->offset += count;
            if (count <= 0 || writer->offset == writer->bytes.size()) {
                ::close(writer->fd);
                writer->fd = -1;
                timer->deleteLater();
            }
        });
        // Reply metadata arrives before a deliberately fragmented pixel stream.
        QTimer::singleShot(75, timer, qOverload<>(&QTimer::start));
        return metadata;
    }
};

class KWinWindowCaptureTest : public QObject {
    Q_OBJECT
private:
    ScreenshotService service;
    QDBusConnection bus = QDBusConnection::sessionBus();

private Q_SLOTS:
    void initTestCase()
    {
        if (!qEnvironmentVariableIsSet("SCREENIES_TEST_PRIVATE_BUS")) {
            QSKIP("Must run on a private test bus; never replace the desktop's screenshot service.");
        }
        QVERIFY(bus.registerService(QStringLiteral("org.kde.KWin.ScreenShot2")));
        QVERIFY(bus.registerObject(QStringLiteral("/org/kde/KWin/ScreenShot2"), &service, QDBusConnection::ExportAllSlots));
    }

    void cleanup()
    {
        service.held.clear();
        service.handle.clear();
        service.options.clear();
    }

    void desktopPermission()
    {
        QFile desktop(QStringLiteral(TEST_DESKTOP_FILE));
        QVERIFY(desktop.open(QIODevice::ReadOnly));
        QVERIFY(desktop.readAll().contains("X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2"));
    }

    void isolatedImage_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("fragmented-alpha") << QStringLiteral("valid");
        QTest::newRow("declared-straight-alpha") << QStringLiteral("straight");
        QTest::newRow("larger-than-pipe-buffer") << QStringLiteral("large");
    }

    void isolatedImage()
    {
        QFETCH(QString, mode);
        service.mode = mode;
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&]() { ++ticks; });
        heartbeat.start();
        QFutureWatcher<X11::CompositorWindowImage> watcher;
        const auto connection = bus;
        watcher.setFuture(QtConcurrent::run([connection]() { return X11::captureKWinWindow(1234, true, false, true, connection, 5000); }));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(), 6000);
        const auto result = watcher.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.image.size(), service.expected.size());
        QCOMPARE(result.image.format(), service.expected.format());
        QCOMPARE(result.image.pixel(0, 0), service.expected.pixel(0, 0));
        QCOMPARE(result.image.devicePixelRatio(), 2.0);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(result.image.save(&buffer, "PNG"));
        const auto saved = QImage::fromData(encoded, "PNG");
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.pixelColor(0, 0), QColor(255, 0, 0, 128));
        QImage composed(1, 1, QImage::Format_ARGB32_Premultiplied);
        composed.fill(Qt::blue);
        QPainter painter(&composed);
        painter.drawImage(QPoint(0, 0), saved);
        painter.end();
        QCOMPARE(composed.pixelColor(0, 0), QColor(128, 0, 127));
        QCOMPARE(service.handle, QStringLiteral("1234"));
        QCOMPARE(service.options.value(QStringLiteral("include-decoration")).toBool(), true);
        QCOMPARE(service.options.value(QStringLiteral("include-shadow")).toBool(), false);
        QCOMPARE(service.options.value(QStringLiteral("include-cursor")).toBool(), true);
        QCOMPARE(service.options.value(QStringLiteral("native-resolution")).toBool(), true);
        QVERIFY(ticks >= 2);
    }

    void failsClosed_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto name : {"denied", "truncated", "timeout", "bad-type", "bad-format", "bad-stride", "huge", "zero-size", "bad-scale"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void failsClosed()
    {
        QFETCH(QString, mode);
        service.mode = mode;
        const auto connection = bus;
        QFutureWatcher<X11::CompositorWindowImage> watcher;
        watcher.setFuture(QtConcurrent::run([connection]() { return X11::captureKWinWindow(1234, true, false, false, connection, 300); }));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(), 3000);
        const auto result = watcher.result();
        QVERIFY(result.image.isNull());
        QVERIFY(!result.error.isEmpty());
        if (mode == QLatin1String("denied")) {
            QVERIFY(result.error.contains(QLatin1String("NoAuthorized")));
        }
    }

    void windowUnderCursorUsesCompositor_data()
    {
        QTest::addColumn<bool>("decorations");
        QTest::newRow("with-frame") << true;
        QTest::newRow("client-only") << false;
    }

    void windowUnderCursorUsesCompositor()
    {
        if (!qEnvironmentVariableIsSet("SCREENIES_TEST_PRIVATE_X11"))
            QSKIP("Requires a private X11 test display");
        QFETCH(bool, decorations);
        service.mode = QStringLiteral("valid");
        QWidget window;
        window.setWindowTitle(QStringLiteral("Screenies test client"));
        window.setGeometry(100, 100, 180, 120);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* connection = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()->connection();
        auto* atom = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, 8, "WM_STATE"), nullptr);
        QVERIFY(atom);
        const quint32 state[] = {1, XCB_NONE};
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window.winId(), atom->atom, atom->atom, 32, 2, state);
        std::free(atom);
        const auto root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
        xcb_warp_pointer(connection, XCB_NONE, root, 0, 0, 0, 0, 150, 150);
        xcb_flush(connection);

        ImagePlatformXcb platform;
        QSignalSpy screenshots(&platform, &ImagePlatform::newScreenshotTaken);
        QSignalSpy failures(&platform, &ImagePlatform::newScreenshotFailed);
        platform.doGrab(ImagePlatform::Immediate, ImagePlatform::WindowUnderCursor, false, decorations, false);
        QTRY_COMPARE_WITH_TIMEOUT(screenshots.count(), 1, 3000);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(service.handle, QString::number(window.winId()));
        QCOMPARE(service.options.value(QStringLiteral("include-decoration")).toBool(), decorations);
        QCOMPARE(service.options.value(QStringLiteral("include-shadow")).toBool(), false);
        const auto image = qvariant_cast<QImage>(screenshots.first().first());
        // Not the client's native geometry or XComposite pixels: the backend
        // must deliver the compositor-rendered image (including its alpha).
        QCOMPARE(image.size(), QSize(6, 4));
        QCOMPARE(image.pixel(0, 0), QRgb(0x80800000));

        screenshots.clear();
        service.handle.clear();
        platform.doGrab(ImagePlatform::Immediate, ImagePlatform::WindowUnderCursor, false, decorations, false);
        const auto watchers = platform.findChildren<QFutureWatcherBase*>();
        QVERIFY(!watchers.isEmpty());
        auto* watcher = watchers.constLast();
        QSignalSpy finished(watcher, &QFutureWatcherBase::finished);
        platform.cancelTargetSelection();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QCOMPARE(screenshots.count(), 0); // Ignore stale replies after cancellation.
    }
};

QTEST_MAIN(KWinWindowCaptureTest)
#include "KWinWindowCaptureTest.moc"
