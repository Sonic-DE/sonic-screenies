/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/X11/X11TargetSelector.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

#include <cstdlib>
#include <cstring>

#include <xcb/xcb.h>

using namespace Qt::StringLiterals;

class X11TargetSelectorTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cancelIsIdempotent();
    void screenHelperResolvesPrimaryScreen();
    void windowHelpersResolveClientTitleAndGeometry();
};

namespace {

class XcbWindow {
public:
    XcbWindow(xcb_connection_t* connection, xcb_window_t parent, const QRect& geometry)
        : m_connection(connection)
        , m_window(xcb_generate_id(connection))
    {
        const uint32_t values[] = {1, XCB_EVENT_MASK_EXPOSURE};
        xcb_create_window(connection,
            XCB_COPY_FROM_PARENT,
            m_window,
            parent,
            geometry.x(),
            geometry.y(),
            geometry.width(),
            geometry.height(),
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            XCB_COPY_FROM_PARENT,
            XCB_CW_EVENT_MASK | XCB_CW_OVERRIDE_REDIRECT,
            values);
    }

    ~XcbWindow()
    {
        if (m_connection && m_window != XCB_NONE) {
            xcb_destroy_window(m_connection, m_window);
            xcb_flush(m_connection);
        }
    }

    xcb_window_t window() const
    {
        return m_window;
    }

private:
    xcb_connection_t* m_connection = nullptr;
    xcb_window_t m_window = XCB_NONE;
};

xcb_atom_t internAtom(xcb_connection_t* connection, const char* name, bool onlyIfExists = false)
{
    const auto cookie = xcb_intern_atom(connection, onlyIfExists, std::strlen(name), name);
    auto reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    const auto atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

void skipUnlessXcb()
{
    if (QGuiApplication::platformName() != u"xcb"_s || !X11::TargetSelector::nativeConnection()) {
        QSKIP("This test requires Qt's xcb platform");
    }
}

} // namespace

void X11TargetSelectorTest::cancelIsIdempotent()
{
    X11::TargetSelector selector(X11::TargetSelectionMode::Window);
    QSignalSpy selectedSpy(&selector, &X11::TargetSelector::targetSelected);
    QSignalSpy canceledSpy(&selector, &X11::TargetSelector::selectionCanceled);
    QVERIFY(selectedSpy.isValid());
    QVERIFY(canceledSpy.isValid());

    selector.cancel(u"first"_s);
    selector.cancel(u"second"_s);

    QCOMPARE(selectedSpy.size(), 0);
    QCOMPARE(canceledSpy.size(), 1);
    QCOMPARE(canceledSpy.first().first().toString(), u"first"_s);
}

void X11TargetSelectorTest::screenHelperResolvesPrimaryScreen()
{
    skipUnlessXcb();

    auto screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);

    const auto nativeGeometry = X11::TargetSelector::nativeGeometryForScreen(screen);
    QVERIFY(!nativeGeometry.isEmpty());
    QCOMPARE(X11::TargetSelector::screenAtNativePosition(nativeGeometry.center()), screen);
}

void X11TargetSelectorTest::windowHelpersResolveClientTitleAndGeometry()
{
    skipUnlessXcb();

    auto connection = X11::TargetSelector::nativeConnection();
    const auto root = X11::TargetSelector::defaultRootWindow(connection);
    QVERIFY(connection);
    QVERIFY(root != XCB_NONE);

    XcbWindow frame(connection, root, QRect(40, 50, 240, 180));
    XcbWindow client(connection, frame.window(), QRect(7, 9, 120, 90));

    const auto wmState = internAtom(connection, "WM_STATE");
    QVERIFY(wmState != XCB_ATOM_NONE);
    const uint32_t wmStateData[] = {1, XCB_NONE};
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, client.window(), wmState, wmState, 32, 2, wmStateData);

    const auto netWmName = internAtom(connection, "_NET_WM_NAME");
    const auto utf8String = internAtom(connection, "UTF8_STRING");
    QVERIFY(netWmName != XCB_ATOM_NONE);
    QVERIFY(utf8String != XCB_ATOM_NONE);
    const QByteArray title = "Selector Test Window";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, client.window(), netWmName, utf8String, 8, title.size(), title.constData());

    xcb_map_window(connection, client.window());
    xcb_map_window(connection, frame.window());
    xcb_flush(connection);

    QCOMPARE(X11::TargetSelector::topLevelClientForWindow(connection, root, frame.window()), client.window());
    QCOMPARE(X11::TargetSelector::titleForWindow(connection, client.window()), QString::fromUtf8(title));
    QCOMPARE(X11::TargetSelector::rootNativeGeometryForWindow(connection, root, client.window()), QRect(47, 59, 120, 90));
}

QTEST_MAIN(X11TargetSelectorTest)
#include "X11TargetSelectorTest.moc"
