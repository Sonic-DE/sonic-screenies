/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/X11/X11WindowCapture.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QtGui/qguiapplication_platform.h>

#include <KWindowInfo>

#include <cmath>

class Target : public QWidget {
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#ca3155")));
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(140, 80, 120, 80), Qt::transparent);
    }
};

class Backdrop : public QWidget {
public:
    QColor color = Qt::blue;
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), color);
    }
};

static QImage recompose(const QImage& foreground, QColor background)
{
    QImage result(foreground.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(background);
    QPainter painter(&result);
    painter.drawImage(0, 0, foreground);
    painter.end();
    return result;
}

static bool sameRgb(QColor left, QColor right)
{
    return std::abs(left.red() - right.red()) <= 2 && std::abs(left.green() - right.green()) <= 2
        && std::abs(left.blue() - right.blue()) <= 2;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (app.arguments().size() != 2 || !qEnvironmentVariableIsSet("SCREENIES_REAL_COMPOSITOR_TEST")) {
        qCritical() << "Run only using the private-display test wrapper";
        return 2;
    }
    const QDir output(app.arguments().at(1));
    for (const auto name : {"frame-blue.png", "frame-green.png", "client.png", "live-blue.png", "live-green.png", "recomposed-blue.png", "recomposed-green.png", "result.json"}) {
        const auto file = output.filePath(QString::fromLatin1(name));
        if (QFile::exists(file) && !QFile::remove(file)) {
            qCritical() << "FAIL: cannot remove previous test artifact" << file;
            return 1;
        }
    }
    Backdrop behind;
    behind.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnBottomHint | Qt::WindowDoesNotAcceptFocus);
    behind.setGeometry(0, 0, 1024, 768);
    behind.show();
    Target target;
    target.setAttribute(Qt::WA_TranslucentBackground);
    target.setWindowTitle(QStringLiteral("Screenies translucent-frame regression fixture"));
    target.setGeometry(100, 100, 400, 240);
    target.show();
    target.raise();

    QTimer readiness;
    QProcess screenshot;
    QRect frame, client;
    QImage blueCapture, blueScene;
    int attempts = 0, stage = 0;
    bool passed = true;
    QJsonObject metrics;
    const QPoint sample(40, 14);
    auto path = [&](int number) {
        if (number == 0) {
            return output.filePath(QStringLiteral("frame-blue.png"));
        }
        if (number == 1) {
            return output.filePath(QStringLiteral("frame-green.png"));
        }
        return output.filePath(QStringLiteral("client.png"));
    };
    auto launch = [&]() {
        if (QFile::exists(path(stage)) && !QFile::remove(path(stage))) {
            qCritical() << "FAIL: cannot remove the previous capture" << path(stage);
            app.exit(1);
            return;
        }
        QStringList arguments = {QStringLiteral("--new-instance"), QStringLiteral("--windowundercursor"), QStringLiteral("--background"),
            QStringLiteral("--nonotify"), QStringLiteral("--no-shadow"), QStringLiteral("--output"), path(stage)};
        if (stage == 2) {
            arguments << QStringLiteral("--no-decoration");
        }
        screenshot.setProcessChannelMode(QProcess::ForwardedChannels);
        screenshot.start(QStringLiteral(SCREENIES_BINARY), arguments);
    };
    QObject::connect(&screenshot, &QProcess::finished, &app, [&](int exitCode, QProcess::ExitStatus status) {
        const QImage image(path(stage));
        if (exitCode != 0 || status != QProcess::NormalExit || image.isNull()) {
            qCritical() << "FAIL: Screenies did not produce a valid PNG" << stage << exitCode;
            app.exit(1);
            return;
        }
        if (stage < 2 && image.size() != frame.size()) {
            qCritical() << "FAIL: decorated capture geometry" << image.size() << "expected" << frame.size();
            app.exit(1);
            return;
        }
        if (stage == 0) {
            blueCapture = image;
            blueScene = app.primaryScreen()->grabWindow(0).toImage();
            blueScene.save(output.filePath(QStringLiteral("live-blue.png")));
            int partial = 0, tested = 0;
            for (int y = 4; y < client.y() - frame.y() - 4; ++y) {
                for (int x = 4; x < image.width() - 4; ++x) {
                    const int alpha = qAlpha(image.pixel(x, y));
                    ++tested;
                    if (alpha > 0 && alpha < 255) {
                        ++partial;
                    }
                }
            }
            const int alpha = qAlpha(image.pixel(sample));
            passed &= tested > 0 && partial > tested / 2 && std::abs(alpha - 128) <= 1;
            passed &= qAlpha(image.pixel(client.center() - frame.topLeft())) == 0;
            metrics[QStringLiteral("sampleX")] = sample.x();
            metrics[QStringLiteral("sampleY")] = sample.y();
            metrics[QStringLiteral("liveSampleX")] = frame.x() + sample.x();
            metrics[QStringLiteral("liveSampleY")] = frame.y() + sample.y();
            metrics[QStringLiteral("sampleAlpha")] = alpha;
            metrics[QStringLiteral("savedBlueRGBA")] = image.pixelColor(sample).name(QColor::HexArgb);
            metrics[QStringLiteral("partialFramePixels")] = partial;
            metrics[QStringLiteral("testedInteriorTitlebarPixels")] = tested;
            qInfo() << "FRAME ALPHA partial=" << partial << "/" << tested << "sample=" << image.pixelColor(sample).name(QColor::HexArgb)
                    << "client=" << image.pixelColor(client.topLeft() - frame.topLeft() + QPoint(25, 25)).name(QColor::HexArgb);
            behind.color = Qt::green;
            behind.repaint();
            target.raise();
            ++stage;
            QTimer::singleShot(100, &app, launch);
        } else if (stage == 1) {
            const auto greenScene = app.primaryScreen()->grabWindow(0).toImage();
            greenScene.save(output.filePath(QStringLiteral("live-green.png")));
            int changed = 0;
            const QRect clientInImage(client.topLeft() - frame.topLeft(), client.size());
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (!clientInImage.contains(x, y) && image.pixel(x, y) != blueCapture.pixel(x, y)) {
                        ++changed;
                    }
                }
            }
            const auto liveBlue = blueScene.pixelColor(frame.topLeft() + sample);
            const auto liveGreen = greenScene.pixelColor(frame.topLeft() + sample);
            const auto recomposedBlue = recompose(blueCapture, Qt::blue);
            const auto recomposedGreen = recompose(image, Qt::green);
            recomposedBlue.save(output.filePath(QStringLiteral("recomposed-blue.png")));
            recomposedGreen.save(output.filePath(QStringLiteral("recomposed-green.png")));
            const bool fidelity = sameRgb(recomposedBlue.pixelColor(sample), liveBlue) && sameRgb(recomposedGreen.pixelColor(sample), liveGreen);
            passed &= changed == 0 && liveBlue != liveGreen && fidelity;
            metrics[QStringLiteral("changedSavedFramePixels")] = changed;
            metrics[QStringLiteral("savedGreenRGBA")] = image.pixelColor(sample).name(QColor::HexArgb);
            metrics[QStringLiteral("liveBlueRGB")] = liveBlue.name();
            metrics[QStringLiteral("liveGreenRGB")] = liveGreen.name();
            metrics[QStringLiteral("recomposedBlueRGB")] = recomposedBlue.pixelColor(sample).name();
            metrics[QStringLiteral("recomposedGreenRGB")] = recomposedGreen.pixelColor(sample).name();
            metrics[QStringLiteral("colorFidelity")] = fidelity;
            qInfo() << "BACKGROUND EXCLUSION changed-frame-pixels=" << changed << "live-blue=" << liveBlue.name() << "live-green=" << liveGreen.name();
            qInfo() << "COLOR FIDELITY" << fidelity << "recomposed-blue=" << recomposedBlue.pixelColor(sample).name()
                    << "recomposed-green=" << recomposedGreen.pixelColor(sample).name();
            ++stage;
            launch();
        } else {
            passed &= image.size() == client.size() && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 0;
            metrics[QStringLiteral("clientHoleAlpha")] = qAlpha(image.pixel(image.width() / 2, image.height() / 2));
            metrics[QStringLiteral("passed")] = passed;
            QFile report(output.filePath(QStringLiteral("result.json")));
            const auto encoded = QJsonDocument(metrics).toJson(QJsonDocument::Indented);
            if (!report.open(QIODevice::WriteOnly) || report.write(encoded) != encoded.size()) {
                qCritical() << "FAIL: cannot write measured results";
                passed = false;
            }
            qInfo() << "REAL TRANSLUCENT-FRAME RESULT" << (passed ? "PASS" : "FAIL") << "artifacts=" << output.absolutePath();
            app.exit(passed ? 0 : 1);
        }
    });
    QObject::connect(&readiness, &QTimer::timeout, &app, [&]() {
        if (++attempts > 200) {
            qCritical() << "FAIL: compositor/window readiness timeout";
            app.exit(2);
            return;
        }
        const QDBusInterface compositor(QStringLiteral("org.kde.KWin"), QStringLiteral("/Compositor"), QString(), QDBusConnection::sessionBus());
        if (!compositor.isValid() || !compositor.property("active").toBool()) {
            return;
        }
        const KWindowInfo info(target.winId(), NET::WMGeometry | NET::WMFrameExtents);
        client = info.geometry();
        frame = info.frameGeometry();
        if (frame.height() <= client.height()) {
            return;
        }
        auto* connection = app.nativeInterface<QNativeInterface::QX11Application>()->connection();
        const auto surface = X11::captureWindow(connection, target.winId(), false);
        if (surface.image.isNull() || (surface.image.pixel(25, 25) & 0xffffff) != 0xca3155) {
            return;
        }
        readiness.stop();
        const auto root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
        xcb_warp_pointer(connection, XCB_NONE, root, 0, 0, 0, 0, client.center().x(), client.center().y());
        xcb_flush(connection);
        launch();
    });
    readiness.start(50);
    QTimer::singleShot(30000, &app, [&]() { qCritical() << "FAIL: capture deadline expired"; app.exit(3); });
    return app.exec();
}
