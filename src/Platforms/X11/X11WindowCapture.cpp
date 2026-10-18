/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "X11WindowCapture.h"

#include <QColor>
#include <QPainter>
#include <QRegion>
#include <QSysInfo>

#include <xcb/composite.h>
#include <xcb/shape.h>
#include <xcb/xcb_image.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace X11 {
namespace {

    struct FreeDeleter {
        void operator()(void* pointer) const
        {
            std::free(pointer);
        }
    };
    template <typename T>
    using Reply = std::unique_ptr<T, FreeDeleter>;

    struct Pixmap {
        xcb_connection_t* connection;
        xcb_window_t window;
        xcb_pixmap_t id = XCB_NONE;
        ~Pixmap()
        {
            if (id != XCB_NONE) {
                xcb_free_pixmap(connection, id);
            }
            xcb_flush(connection);
        }
        bool name()
        {
            const auto candidate = xcb_generate_id(connection);
            Reply<xcb_generic_error_t> error(xcb_request_check(connection,
                xcb_composite_name_window_pixmap_checked(connection, window, candidate)));
            if (error) {
                return false;
            }
            id = candidate;
            return true;
        }
    };

    WindowImage readWindow(xcb_connection_t* connection, xcb_window_t window)
    {
        Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(connection,
            xcb_get_geometry(connection, window), nullptr));
        if (!geometry || window == geometry->root || !geometry->width || !geometry->height) {
            return {};
        }
        Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(connection,
            xcb_get_window_attributes(connection, window), nullptr));
        if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
            return {};
        }
        Reply<xcb_translate_coordinates_reply_t> position(xcb_translate_coordinates_reply(connection,
            xcb_translate_coordinates(connection, window, geometry->root, 0, 0), nullptr));
        if (!position || !position->same_screen) {
            return {};
        }

        Pixmap pixmap{connection, window};
        for (int depth = 0; !pixmap.name(); ++depth) {
            // SonicWin redirects the WM frame hierarchy, not necessarily each
            // client separately. Read that existing isolated backing store, never
            // the root or a newly-created redirection with undefined initial data.
            Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection,
                xcb_query_tree(connection, pixmap.window), nullptr));
            if (depth >= 64 || !tree || tree->parent == tree->root || tree->parent == XCB_NONE) {
                return {};
            }
            pixmap.window = tree->parent;
        }

        Reply<xcb_get_geometry_reply_t> sourceGeometry(xcb_get_geometry_reply(connection,
            xcb_get_geometry(connection, pixmap.window), nullptr));
        Reply<xcb_translate_coordinates_reply_t> localPosition(xcb_translate_coordinates_reply(connection,
            xcb_translate_coordinates(connection, window, pixmap.window, 0, 0), nullptr));
        if (!sourceGeometry || !localPosition || !localPosition->same_screen) {
            return {};
        }

        // Named pixmaps include the X border. Window geometry and cursor positions
        // refer to the interior, so read in local coordinates, not root coordinates.
        auto* native = xcb_image_get(connection, pixmap.id,
            localPosition->dst_x + sourceGeometry->border_width, localPosition->dst_y + sourceGeometry->border_width,
            geometry->width, geometry->height, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP);
        if (!native) {
            return {};
        }
        const auto destroyImage = [](xcb_image_t* image) { xcb_image_destroy(image); };
        std::unique_ptr<xcb_image_t, decltype(destroyImage)> ownedImage(native, destroyImage);
        const auto nativeOrder = QSysInfo::ByteOrder == QSysInfo::LittleEndian ? XCB_IMAGE_ORDER_LSB_FIRST : XCB_IMAGE_ORDER_MSB_FIRST;
        if (!native->data || native->bpp != 32 || native->byte_order != nativeOrder
            || (native->depth != 24 && native->depth != 32)
            || (geometry->depth != 24 && geometry->depth != 32)
            || (geometry->depth == 32 && native->depth != 32)) {
            return {};
        }
        // Reject unfamiliar channel layouts rather than interpreting them as ARGB.
        bool standardVisual = false;
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
        for (; screens.rem; xcb_screen_next(&screens)) {
            auto depths = xcb_screen_allowed_depths_iterator(screens.data);
            for (; depths.rem; xcb_depth_next(&depths)) {
                auto visuals = xcb_depth_visuals_iterator(depths.data);
                for (; visuals.rem; xcb_visualtype_next(&visuals)) {
                    if (visuals.data->visual_id == attributes->visual) {
                        standardVisual = visuals.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR
                            && visuals.data->red_mask == 0xff0000 && visuals.data->green_mask == 0xff00
                            && visuals.data->blue_mask == 0xff;
                    }
                }
            }
        }
        if (!standardVisual) {
            return {};
        }
        const auto format = geometry->depth == 32 ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
        QImage image = QImage(native->data, native->width, native->height, native->stride, format).copy();
        if (image.isNull()) {
            return {};
        }
        if (geometry->depth == 24) {
            // XRGB's unused byte is not an alpha channel.
            for (int y = 0; y < image.height(); ++y) {
                auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    pixels[x] |= 0xff000000;
                }
            }
        }
        const auto* shape = xcb_get_extension_data(connection, &xcb_shape_id);
        if (shape && shape->present) {
            Reply<xcb_shape_get_rectangles_reply_t> bounds(xcb_shape_get_rectangles_reply(connection,
                xcb_shape_get_rectangles(connection, window, XCB_SHAPE_SK_BOUNDING), nullptr));
            if (!bounds) {
                return {};
            }
            QRegion region;
            const auto* rectangles = xcb_shape_get_rectangles_rectangles(bounds.get());
            for (int i = 0; i < xcb_shape_get_rectangles_rectangles_length(bounds.get()); ++i) {
                const auto& rect = rectangles[i];
                region += QRect(rect.x, rect.y, rect.width, rect.height);
            }
            QImage shaped(image.size(), QImage::Format_ARGB32_Premultiplied);
            if (shaped.isNull()) {
                return {};
            }
            shaped.fill(Qt::transparent);
            QPainter painter(&shaped);
            painter.setClipRegion(region);
            painter.drawImage(0, 0, image);
            painter.end();
            image = shaped;
        }
        return {image, QPoint(position->dst_x, position->dst_y)};
    }

    WindowImage applyOpacity(xcb_connection_t* connection, xcb_window_t window, WindowImage capture)
    {
        if (capture.image.isNull()) {
            return capture;
        }
        constexpr char propertyName[] = "_NET_WM_WINDOW_OPACITY";
        Reply<xcb_intern_atom_reply_t> atom(xcb_intern_atom_reply(connection,
            xcb_intern_atom(connection, true, sizeof(propertyName) - 1, propertyName), nullptr));
        if (!atom || atom->atom == XCB_ATOM_NONE) {
            return capture;
        }
        Reply<xcb_get_property_reply_t> property(xcb_get_property_reply(connection,
            xcb_get_property(connection, false, window, atom->atom, XCB_ATOM_CARDINAL, 0, 1), nullptr));
        if (!property) {
            return {};
        }
        if (property->type == XCB_ATOM_CARDINAL && property->format == 32 && xcb_get_property_value_length(property.get()) == 4) {
            const auto opacity = *static_cast<const uint32_t*>(xcb_get_property_value(property.get()));
            if (opacity != std::numeric_limits<uint32_t>::max()) {
                capture.image = capture.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                QPainter painter(&capture.image);
                painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                painter.fillRect(capture.image.rect(), QColor::fromRgbF(0, 0, 0, double(opacity) / std::numeric_limits<uint32_t>::max()));
            }
        }
        return capture;
    }

} // namespace

WindowImage captureWindow(xcb_connection_t* connection, xcb_window_t window, bool includeDecorations)
{
    if (!connection || xcb_connection_has_error(connection) || window == XCB_NONE) {
        return {};
    }
    const auto* composite = xcb_get_extension_data(connection, &xcb_composite_id);
    if (!composite || !composite->present) {
        return {};
    }
    Reply<xcb_composite_query_version_reply_t> version(xcb_composite_query_version_reply(connection,
        xcb_composite_query_version(connection, 0, 4), nullptr));
    if (!version || (version->major_version == 0 && version->minor_version < 2)) {
        return {};
    }
    auto client = readWindow(connection, window);
    if (client.image.isNull() || !includeDecorations) {
        return applyOpacity(connection, window, std::move(client));
    }
    auto frame = window;
    // A broken or changing parent tree must not let us walk into the root.
    for (int depth = 0; depth < 64; ++depth) {
        Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection, xcb_query_tree(connection, frame), nullptr));
        if (!tree || frame == tree->root || tree->parent == XCB_NONE) {
            return {};
        }
        if (tree->parent == tree->root) {
            if (frame == window) {
                return applyOpacity(connection, window, std::move(client));
            }
            auto decorated = readWindow(connection, frame);
            if (decorated.image.isNull()) {
                return {};
            }
            decorated.image = decorated.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            // Redirected client and decoration surfaces may be separate. Replace
            // the entire client rectangle, INCLUDING transparent pixels, so the
            // frame's backing store cannot fill the transparent client area.
            QPainter painter(&decorated.image);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(client.position - decorated.position, client.image);
            painter.end();
            return applyOpacity(connection, window, std::move(decorated));
        }
        frame = tree->parent;
    }
    return {};
}

} // namespace X11
