/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "ScreenshotTiming.h"

#include <QtTest>

class ScreenshotTimingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void captureDelay_data();
    void captureDelay();
};

void ScreenshotTimingTest::captureDelay_data()
{
    QTest::addColumn<int>("requestedDelay");
    QTest::addColumn<bool>("isX11");
    QTest::addColumn<int>("expectedDelay");

    // A zero-delay current-screen capture must not grab while the hidden
    // screenshot viewer is still fading out in the compositor.
    QTest::newRow("immediate-x11") << 0 << true << 500;
    QTest::newRow("old-50ms-x11-delay") << 50 << true << 500;
    QTest::newRow("short-x11-countdown") << 250 << true << 500;
    QTest::newRow("settled-x11") << 500 << true << 500;
    QTest::newRow("long-x11-countdown") << 2000 << true << 2000;
    QTest::newRow("immediate-other-platform") << 0 << false << 0;
    QTest::newRow("other-platform-countdown") << 250 << false << 250;
    QTest::newRow("negative-delay") << -1 << false << 0;
}

void ScreenshotTimingTest::captureDelay()
{
    QFETCH(int, requestedDelay);
    QFETCH(bool, isX11);
    QFETCH(int, expectedDelay);

    QCOMPARE(ScreenshotTiming::captureDelay(requestedDelay, isX11), expectedDelay);
}

QTEST_GUILESS_MAIN(ScreenshotTimingTest)

#include "ScreenshotTimingTest.moc"
