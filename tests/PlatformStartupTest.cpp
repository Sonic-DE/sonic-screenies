/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "PlatformStartup.h"

#include <QObject>
#include <QtTest>

class PlatformStartupTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void acceptsXcbWithEmptySession();
    void acceptsXcbWithX11Session();
    void rejectsXcbWithWaylandSession();
    void rejectsNativeWaylandPlatform();
    void rejectsOtherPlatforms();
};

void PlatformStartupTest::acceptsXcbWithEmptySession()
{
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("xcb"), QString()), true);
}

void PlatformStartupTest::acceptsXcbWithX11Session()
{
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("xcb"), QStringLiteral("x11")), true);
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("xcb"), QStringLiteral("X11")), true);
}

void PlatformStartupTest::rejectsXcbWithWaylandSession()
{
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("xcb"), QStringLiteral("wayland")), false);
}

void PlatformStartupTest::rejectsNativeWaylandPlatform()
{
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("wayland"), QString()), false);
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("wayland"), QStringLiteral("x11")), false);
}

void PlatformStartupTest::rejectsOtherPlatforms()
{
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral("minimal"), QString()), false);
    QCOMPARE(PlatformStartup::isSupportedX11Session(QStringLiteral(), QString()), false);
}

QTEST_GUILESS_MAIN(PlatformStartupTest)
#include "PlatformStartupTest.moc"
