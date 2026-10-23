/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Platforms/X11/X11WindowCapture.h"

#include <QBuffer>
#include <QtTest>
#include <xcb/composite.h>
#include <xcb/shape.h>
#include <xcb/xcb_image.h>

#include <cstdlib>
#include <memory>

namespace {

class Window {
public:
    Window(xcb_connection_t* connection, xcb_screen_t* screen, xcb_window_t parent,
        QRect geometry, bool alpha, bool redirected = true)
        : connection(connection)
        , id(xcb_generate_id(connection))
    {
        auto visual = screen->root_visual;
        const uint8_t depth = alpha ? 32 : screen->root_depth;
        if (alpha) {
            auto depths = xcb_screen_allowed_depths_iterator(screen);
            for (; depths.rem; xcb_depth_next(&depths)) {
                if (depths.data->depth != 32) {
                    continue;
                }
                auto visuals = xcb_depth_visuals_iterator(depths.data);
                for (; visuals.rem; xcb_visualtype_next(&visuals)) {
                    if (visuals.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR
                        && visuals.data->red_mask == 0xff0000 && visuals.data->green_mask == 0xff00
                        && visuals.data->blue_mask == 0xff) {
                        visual = visuals.data->visual_id;
                    }
                }
            }
        }
        colormap = xcb_generate_id(connection);
        xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root, visual);
        const uint32_t values[] = {0, 0, 1, colormap};
        auto* error = xcb_request_check(connection, xcb_create_window_checked(connection, depth, id, parent, geometry.x(), geometry.y(), geometry.width(), geometry.height(), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP, values));
        valid = !error;
        std::free(error);
        if (valid && redirected) {
            error = xcb_request_check(connection,
                xcb_composite_redirect_window_checked(connection, id, XCB_COMPOSITE_REDIRECT_MANUAL));
            valid = !error;
            std::free(error);
        }
        xcb_map_window(connection, id);
    }
    ~Window()
    {
        xcb_destroy_window(connection, id);
        xcb_free_colormap(connection, colormap);
        xcb_flush(connection);
    }
    void fill(uint32_t pixel, QSize size)
    {
        const auto gc = xcb_generate_id(connection);
        xcb_create_gc(connection, gc, id, XCB_GC_FOREGROUND, &pixel);
        const xcb_rectangle_t rectangle = {0, 0, uint16_t(size.width()), uint16_t(size.height())};
        xcb_poly_fill_rectangle(connection, id, gc, 1, &rectangle);
        xcb_free_gc(connection, gc);
        xcb_flush(connection);
    }
    xcb_connection_t* connection;
    xcb_window_t id;
    xcb_colormap_t colormap;
    bool valid = false;
};

} // namespace

class X11WindowCaptureTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void preservesAlphaAndExcludesOtherWindows();
    void decorationsDoNotFillTransparentClient();
    void frameHierarchyRetainsClientAlpha();
    void preservesWindowOpacity();
    void shapedCornersRemainTransparent();
    void invalidUnmappedAndUnredirectedWindowsFailClosed();

private:
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
};

void X11WindowCaptureTest::initTestCase()
{
    connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        QSKIP("Requires an isolated X11 test display (for example xvfb-run).");
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    const auto* composite = xcb_get_extension_data(connection, &xcb_composite_id);
    if (qEnvironmentVariableIsSet("SCREENIES_TEST_NO_COMPOSITE")) {
        QVERIFY(!composite || !composite->present);
        return;
    }
    QVERIFY(composite && composite->present);
    auto* version = xcb_composite_query_version_reply(connection,
        xcb_composite_query_version(connection, 0, 4), nullptr);
    QVERIFY(version);
    std::free(version);
}

void X11WindowCaptureTest::cleanupTestCase()
{
    xcb_disconnect(connection);
}

void X11WindowCaptureTest::preservesAlphaAndExcludesOtherWindows()
{
    Window behind(connection, screen, screen->root, QRect(30, 40, 100, 80), false, false);
    QVERIFY(behind.valid);
    behind.fill(0x0000ff, QSize(100, 80));
    Window target(connection, screen, screen->root, QRect(40, 50, 31, 17), true);
    QVERIFY(target.valid);
    target.fill(0x80800000, QSize(31, 17)); // Premultiplied 50%-opaque red.
    Window covering(connection, screen, screen->root, QRect(40, 50, 31, 17), false, false);
    QVERIFY(covering.valid);
    covering.fill(0x0000ff, QSize(31, 17));
    auto* desktop = xcb_image_get(connection, screen->root, 45, 55, 1, 1, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP);
    QVERIFY(desktop);
    const auto desktopPixel = xcb_image_get_pixel(desktop, 0, 0) & 0xffffff;
    xcb_image_destroy(desktop);
    QCOMPARE(desktopPixel, 0x0000ffu); // A desktop crop would expose this blue window.

    for (const bool decorations : {false, true}) {
        const auto capture = X11::captureWindow(connection, target.id, decorations);
        QVERIFY(!capture.image.isNull());
        QCOMPARE(capture.position, QPoint(40, 50));
        QCOMPARE(capture.image.size(), QSize(31, 17));
        QCOMPARE(capture.image.pixel(10, 8), QRgb(0x80800000));
        QBuffer png;
        QVERIFY(png.open(QIODevice::WriteOnly));
        QVERIFY(capture.image.save(&png, "PNG"));
        const auto saved = QImage::fromData(png.data(), "PNG").convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(saved.pixel(10, 8), QRgb(0x80800000));
        // Capturing again also checks that we did not unredirect a surface
        // owned by the compositor (represented by the fixture).
        QVERIFY(!X11::captureWindow(connection, target.id, decorations).image.isNull());
    }
}

void X11WindowCaptureTest::decorationsDoNotFillTransparentClient()
{
    // This fixture paints decoration pixels into an X11 frame. SonicWin's
    // scene-rendered decorations are covered by KWinWindowCaptureTest instead.
    Window frame(connection, screen, screen->root, QRect(100, 120, 90, 70), false);
    QVERIFY(frame.valid);
    frame.fill(0x0000ff, QSize(90, 70));
    Window client(connection, screen, frame.id, QRect(7, 11, 31, 17), true);
    QVERIFY(client.valid);
    client.fill(0, QSize(31, 17));

    const auto capture = X11::captureWindow(connection, client.id, true);
    QVERIFY(!capture.image.isNull());
    QCOMPARE(capture.position, QPoint(100, 120));
    QCOMPARE(capture.image.size(), QSize(90, 70));
    QCOMPARE(capture.image.pixel(1, 1), qRgb(0, 0, 255));
    QCOMPARE(capture.image.pixel(12, 15), QRgb(0));
    QCOMPARE(X11::captureWindow(connection, client.id, false).image.size(), QSize(31, 17));
}

void X11WindowCaptureTest::frameHierarchyRetainsClientAlpha()
{
    // Match SonicWin: the root's frame hierarchy is redirected, while the
    // client is an ordinary child of that redirected frame.
    Window frame(connection, screen, screen->root, QRect(100, 120, 90, 70), true);
    QVERIFY(frame.valid);
    frame.fill(0xff0000ff, QSize(90, 70));
    Window client(connection, screen, frame.id, QRect(7, 11, 31, 17), true, false);
    QVERIFY(client.valid);
    client.fill(0x80800000, QSize(31, 17));
    const auto capture = X11::captureWindow(connection, client.id, false);
    QVERIFY(!capture.image.isNull());
    QCOMPARE(capture.position, QPoint(107, 131));
    QCOMPARE(capture.image.pixel(10, 8), QRgb(0x80800000));
    const auto decorated = X11::captureWindow(connection, client.id, true);
    QVERIFY(!decorated.image.isNull());
    QCOMPARE(decorated.image.pixel(17, 19), QRgb(0x80800000));
}

void X11WindowCaptureTest::preservesWindowOpacity()
{
    Window target(connection, screen, screen->root, QRect(20, 30, 31, 17), false);
    QVERIFY(target.valid);
    target.fill(0xff0000, QSize(31, 17));
    constexpr char name[] = "_NET_WM_WINDOW_OPACITY";
    auto* atom = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, sizeof(name) - 1, name), nullptr);
    QVERIFY(atom);
    const uint32_t opacity = 0x80808080;
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, target.id, atom->atom, XCB_ATOM_CARDINAL, 32, 1, &opacity);
    std::free(atom);
    const auto capture = X11::captureWindow(connection, target.id, false);
    QVERIFY(!capture.image.isNull());
    QCOMPARE(capture.image.pixel(10, 8), QRgb(0x80800000));
}

void X11WindowCaptureTest::shapedCornersRemainTransparent()
{
    Window target(connection, screen, screen->root, QRect(20, 30, 31, 17), false);
    QVERIFY(target.valid);
    target.fill(0xff0000, QSize(31, 17));
    const xcb_rectangle_t rectangle = {5, 5, 20, 10};
    xcb_shape_rectangles(connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
        target.id, 0, 0, 1, &rectangle);
    const auto capture = X11::captureWindow(connection, target.id, false);
    QVERIFY(!capture.image.isNull());
    QCOMPARE(capture.image.pixel(0, 0), QRgb(0));
    QCOMPARE(capture.image.pixel(10, 8), qRgb(255, 0, 0));
}

void X11WindowCaptureTest::invalidUnmappedAndUnredirectedWindowsFailClosed()
{
    QVERIFY(X11::captureWindow(connection, XCB_NONE, true).image.isNull());
    QVERIFY(X11::captureWindow(connection, screen->root, true).image.isNull());
    const auto nonexistent = xcb_generate_id(connection);
    QVERIFY(X11::captureWindow(connection, nonexistent, true).image.isNull());
    Window target(connection, screen, screen->root, QRect(20, 30, 31, 17), false, false);
    QVERIFY(target.valid);
    target.fill(0x0000ff, QSize(31, 17));
    QVERIFY(X11::captureWindow(connection, target.id, false).image.isNull());
    xcb_unmap_window(connection, target.id);
    QVERIFY(X11::captureWindow(connection, target.id, true).image.isNull());
}

QTEST_GUILESS_MAIN(X11WindowCaptureTest)
#include "X11WindowCaptureTest.moc"
