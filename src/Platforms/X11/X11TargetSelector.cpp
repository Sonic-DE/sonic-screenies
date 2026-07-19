/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "X11TargetSelector.h"

#include <QGuiApplication>
#include <QList>
#include <QScreen>
#include <QtGui/qguiapplication_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <xcb/randr.h>
#include <xcb/xcb_cursor.h>

using namespace Qt::StringLiterals;

namespace X11 {

namespace {

    constexpr xcb_keysym_t escapeKeysym = 0xff1b;

    class ReplyPtr {
    public:
        explicit ReplyPtr(void* reply = nullptr)
            : m_reply(reply)
        {
        }

        ~ReplyPtr()
        {
            free(m_reply);
        }

        ReplyPtr(const ReplyPtr&) = delete;
        ReplyPtr& operator=(const ReplyPtr&) = delete;

        template <typename T>
        T* get() const
        {
            return reinterpret_cast<T*>(m_reply);
        }

        explicit operator bool() const
        {
            return m_reply != nullptr;
        }

    private:
        void* m_reply = nullptr;
    };

    xcb_atom_t internAtom(xcb_connection_t* connection, const char* name, bool onlyIfExists = true)
    {
        if (!connection || !name) {
            return XCB_ATOM_NONE;
        }

        const auto cookie = xcb_intern_atom_unchecked(connection, onlyIfExists, std::strlen(name), name);
        ReplyPtr reply(xcb_intern_atom_reply(connection, cookie, nullptr));
        auto atomReply = reply.get<xcb_intern_atom_reply_t>();
        return atomReply ? atomReply->atom : XCB_ATOM_NONE;
    }

    bool hasProperty(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property)
    {
        if (!connection || window == XCB_NONE || property == XCB_ATOM_NONE) {
            return false;
        }

        const auto cookie = xcb_get_property_unchecked(connection, false, window, property, XCB_ATOM_ANY, 0, 0);
        ReplyPtr reply(xcb_get_property_reply(connection, cookie, nullptr));
        auto propReply = reply.get<xcb_get_property_reply_t>();
        return propReply && propReply->type != XCB_ATOM_NONE;
    }

    xcb_screen_t* screenForRoot(xcb_connection_t* connection, xcb_window_t root)
    {
        if (!connection || root == XCB_NONE) {
            return nullptr;
        }

        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
        while (iterator.rem) {
            if (iterator.data->root == root) {
                return iterator.data;
            }
            xcb_screen_next(&iterator);
        }
        return nullptr;
    }

    QByteArray propertyBytes(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type)
    {
        if (!connection || window == XCB_NONE || property == XCB_ATOM_NONE) {
            return {};
        }

        const auto cookie = xcb_get_property_unchecked(connection, false, window, property, type, 0, 1024);
        ReplyPtr reply(xcb_get_property_reply(connection, cookie, nullptr));
        auto propReply = reply.get<xcb_get_property_reply_t>();
        if (!propReply || propReply->type == XCB_ATOM_NONE || propReply->format != 8) {
            return {};
        }

        const auto length = xcb_get_property_value_length(propReply);
        const auto value = xcb_get_property_value(propReply);
        if (!value || length <= 0) {
            return {};
        }
        return QByteArray(static_cast<const char*>(value), length);
    }

    bool isWheelButton(uint8_t detail)
    {
        return detail >= 4 && detail <= 7;
    }

    struct NativeMonitor {
        QString name;
        QRect geometry;
    };

    QList<NativeMonitor> nativeMonitors(xcb_connection_t* connection, xcb_window_t root)
    {
        QList<NativeMonitor> monitors;
        if (!connection || root == XCB_NONE) {
            return monitors;
        }
        const auto cookie = xcb_randr_get_monitors_unchecked(connection, root, true);
        ReplyPtr reply(xcb_randr_get_monitors_reply(connection, cookie, nullptr));
        const auto monitorReply = reply.get<xcb_randr_get_monitors_reply_t>();
        if (!monitorReply) {
            return monitors;
        }
        auto iterator = xcb_randr_get_monitors_monitors_iterator(monitorReply);
        while (iterator.rem) {
            const auto monitor = iterator.data;
            QString name;
            if (monitor->name != XCB_ATOM_NONE) {
                const auto nameCookie = xcb_get_atom_name_unchecked(connection, monitor->name);
                ReplyPtr nameReplyPtr(xcb_get_atom_name_reply(connection, nameCookie, nullptr));
                const auto nameReply = nameReplyPtr.get<xcb_get_atom_name_reply_t>();
                if (nameReply) {
                    name = QString::fromUtf8(xcb_get_atom_name_name(nameReply), xcb_get_atom_name_name_length(nameReply));
                }
            }
            monitors.append({name, QRect(monitor->x, monitor->y, monitor->width, monitor->height)});
            xcb_randr_monitor_info_next(&iterator);
        }
        return monitors;
    }

} // namespace

TargetSelector::TargetSelector(TargetSelectionMode mode, QObject* parent)
    : QObject(parent)
    , m_mode(mode)
{
    qRegisterMetaType<X11::Target>();
}

TargetSelector::~TargetSelector()
{
    if (m_active && !m_terminalSignalEmitted) {
        finishCanceled(u"Target selection was destroyed"_s);
    } else {
        cleanupX11Resources();
    }
}

xcb_connection_t* TargetSelector::nativeConnection()
{
    if (!qGuiApp || QGuiApplication::platformName() != u"xcb"_s) {
        return nullptr;
    }

    auto nativeInterface = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return nativeInterface ? nativeInterface->connection() : nullptr;
}

xcb_window_t TargetSelector::defaultRootWindow(xcb_connection_t* connection)
{
    if (!connection || xcb_connection_has_error(connection)) {
        return XCB_NONE;
    }

    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
    return iterator.rem ? iterator.data->root : XCB_NONE;
}

QRect TargetSelector::nativeGeometryForScreen(const QScreen* screen)
{
    if (!screen) {
        return {};
    }

    const auto connection = nativeConnection();
    const auto monitors = nativeMonitors(connection, defaultRootWindow(connection));
    for (const auto& monitor : monitors) {
        if (monitor.name == screen->name()) {
            return monitor.geometry;
        }
    }

    const auto geometry = screen->geometry();
    const auto scale = screen->devicePixelRatio();
    const QRect scaled(QPoint(std::lround(geometry.x() * scale), std::lround(geometry.y() * scale)),
        QSize(std::lround(geometry.width() * scale), std::lround(geometry.height() * scale)));
    QList<QRect> matches;
    for (const auto& monitor : monitors) {
        if (monitor.geometry == geometry || monitor.geometry == scaled) {
            matches.append(monitor.geometry);
        }
    }
    return matches.size() == 1 ? matches.constFirst() : QRect{};
}

QScreen* TargetSelector::screenAtNativePosition(const QPoint& nativePosition)
{
    const auto screens = QGuiApplication::screens();
    for (auto screen : screens) {
        if (nativeGeometryForScreen(screen).contains(nativePosition)) {
            return screen;
        }
    }
    return nullptr;
}

xcb_window_t TargetSelector::topLevelClientForWindow(xcb_connection_t* connection, xcb_window_t root, xcb_window_t window)
{
    if (!connection || window == XCB_NONE || window == root) {
        return XCB_NONE;
    }

    const auto wmState = internAtom(connection, "WM_STATE");
    if (wmState == XCB_ATOM_NONE) {
        return window;
    }

    xcb_window_t current = window;
    while (current != XCB_NONE && current != root) {
        if (hasProperty(connection, current, wmState)) {
            return current;
        }

        const auto treeCookie = xcb_query_tree_unchecked(connection, current);
        ReplyPtr treeReplyPtr(xcb_query_tree_reply(connection, treeCookie, nullptr));
        auto treeReply = treeReplyPtr.get<xcb_query_tree_reply_t>();
        if (!treeReply) {
            break;
        }
        current = treeReply->parent;
    }

    QList<xcb_window_t> stack;
    stack.append(window);
    while (!stack.isEmpty()) {
        const auto candidate = stack.takeLast();
        if (candidate == XCB_NONE || candidate == root) {
            continue;
        }
        if (hasProperty(connection, candidate, wmState)) {
            return candidate;
        }

        const auto treeCookie = xcb_query_tree_unchecked(connection, candidate);
        ReplyPtr treeReplyPtr(xcb_query_tree_reply(connection, treeCookie, nullptr));
        auto treeReply = treeReplyPtr.get<xcb_query_tree_reply_t>();
        if (!treeReply) {
            continue;
        }

        const auto children = xcb_query_tree_children(treeReply);
        const auto childCount = xcb_query_tree_children_length(treeReply);
        for (int i = 0; i < childCount; ++i) {
            stack.append(children[i]);
        }
    }

    return window;
}

QRect TargetSelector::rootNativeGeometryForWindow(xcb_connection_t* connection, xcb_window_t root, xcb_window_t window)
{
    if (!connection || window == XCB_NONE || root == XCB_NONE) {
        return {};
    }

    const auto geometryCookie = xcb_get_geometry_unchecked(connection, window);
    ReplyPtr geometryReplyPtr(xcb_get_geometry_reply(connection, geometryCookie, nullptr));
    auto geometryReply = geometryReplyPtr.get<xcb_get_geometry_reply_t>();
    if (!geometryReply || geometryReply->width == 0 || geometryReply->height == 0) {
        return {};
    }

    QPoint rootPosition(geometryReply->x, geometryReply->y);
    xcb_window_t current = window;
    while (current != root) {
        const auto treeCookie = xcb_query_tree_unchecked(connection, current);
        ReplyPtr treeReplyPtr(xcb_query_tree_reply(connection, treeCookie, nullptr));
        const auto treeReply = treeReplyPtr.get<xcb_query_tree_reply_t>();
        if (!treeReply || treeReply->parent == XCB_NONE || treeReply->parent == current) {
            return {};
        }
        current = treeReply->parent;
        if (current == root) {
            break;
        }
        const auto parentGeometryCookie = xcb_get_geometry_unchecked(connection, current);
        ReplyPtr parentGeometryReplyPtr(xcb_get_geometry_reply(connection, parentGeometryCookie, nullptr));
        const auto parentGeometryReply = parentGeometryReplyPtr.get<xcb_get_geometry_reply_t>();
        if (!parentGeometryReply) {
            return {};
        }
        rootPosition += QPoint(parentGeometryReply->x, parentGeometryReply->y);
    }
    return QRect(rootPosition, QSize(geometryReply->width, geometryReply->height));
}

QString TargetSelector::titleForWindow(xcb_connection_t* connection, xcb_window_t window)
{
    const auto netWmName = internAtom(connection, "_NET_WM_NAME");
    const auto utf8String = internAtom(connection, "UTF8_STRING");
    auto title = propertyBytes(connection, window, netWmName, utf8String);
    if (!title.isEmpty()) {
        return QString::fromUtf8(title.constData(), title.size());
    }

    title = propertyBytes(connection, window, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
    if (!title.isEmpty()) {
        return QString::fromLocal8Bit(title.constData(), title.size());
    }

    return {};
}

bool TargetSelector::start()
{
    if (m_active) {
        return true;
    }

    m_terminalSignalEmitted = false;
    if (!setupX11Resources() || !grabInput()) {
        finishCanceled(u"Could not start X11 target selection"_s);
        return false;
    }

    qGuiApp->installNativeEventFilter(this);
    m_filterInstalled = true;
    m_active = true;

    xcb_allow_events(m_connection, XCB_ALLOW_SYNC_POINTER, XCB_TIME_CURRENT_TIME);
    xcb_flush(m_connection);
    return true;
}

void TargetSelector::cancel(const QString& reason)
{
    finishCanceled(reason.isEmpty() ? u"Target selection canceled"_s : reason);
}

bool TargetSelector::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result)

    if (!m_active || eventType != "xcb_generic_event_t" || !message) {
        return false;
    }

    const auto event = static_cast<xcb_generic_event_t*>(message);
    switch (event->response_type & ~0x80) {
    case XCB_BUTTON_PRESS:
        return handleButtonPress(reinterpret_cast<xcb_button_press_event_t*>(event));
    case XCB_BUTTON_RELEASE:
        return handleButtonRelease(reinterpret_cast<xcb_button_release_event_t*>(event));
    case XCB_KEY_PRESS:
        return handleKeyPress(reinterpret_cast<xcb_key_press_event_t*>(event));
    default:
        return false;
    }
}

bool TargetSelector::setupX11Resources()
{
    m_connection = nativeConnection();
    m_root = defaultRootWindow(m_connection);
    if (!m_connection || m_root == XCB_NONE) {
        return false;
    }

    auto screen = screenForRoot(m_connection, m_root);
    if (screen && xcb_cursor_context_new(m_connection, screen, &m_cursorContext) >= 0) {
        const QList<QByteArray> cursorNames = {"cross"_ba, "crosshair"_ba, "diamond-cross"_ba, "cross-reverse"_ba};
        for (const auto& cursorName : cursorNames) {
            const auto cursor = xcb_cursor_load_cursor(m_cursorContext, cursorName.constData());
            if (cursor != XCB_CURSOR_NONE) {
                m_cursor = cursor;
                break;
            }
        }
    }

    return true;
}

bool TargetSelector::grabInput()
{
    const auto eventMask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    const auto pointerCookie = xcb_grab_pointer_unchecked(m_connection,
        false,
        m_root,
        eventMask,
        XCB_GRAB_MODE_SYNC,
        XCB_GRAB_MODE_ASYNC,
        XCB_NONE,
        m_cursor,
        XCB_TIME_CURRENT_TIME);
    ReplyPtr pointerReplyPtr(xcb_grab_pointer_reply(m_connection, pointerCookie, nullptr));
    auto pointerReply = pointerReplyPtr.get<xcb_grab_pointer_reply_t>();
    if (!pointerReply || pointerReply->status != XCB_GRAB_STATUS_SUCCESS) {
        return false;
    }
    m_pointerGrabbed = true;

    const auto keyboardCookie = xcb_grab_keyboard_unchecked(m_connection,
        false,
        m_root,
        XCB_TIME_CURRENT_TIME,
        XCB_GRAB_MODE_ASYNC,
        XCB_GRAB_MODE_ASYNC);
    ReplyPtr keyboardReplyPtr(xcb_grab_keyboard_reply(m_connection, keyboardCookie, nullptr));
    auto keyboardReply = keyboardReplyPtr.get<xcb_grab_keyboard_reply_t>();
    if (!keyboardReply || keyboardReply->status != XCB_GRAB_STATUS_SUCCESS) {
        cleanupX11Resources();
        return false;
    }
    m_keyboardGrabbed = true;
    xcb_flush(m_connection);
    return true;
}

void TargetSelector::cleanupX11Resources()
{
    if (m_filterInstalled && qGuiApp) {
        qGuiApp->removeNativeEventFilter(this);
        m_filterInstalled = false;
    }

    if (m_connection) {
        if (m_keyboardGrabbed) {
            xcb_ungrab_keyboard(m_connection, XCB_TIME_CURRENT_TIME);
            m_keyboardGrabbed = false;
        }
        if (m_pointerGrabbed) {
            xcb_ungrab_pointer(m_connection, XCB_TIME_CURRENT_TIME);
            m_pointerGrabbed = false;
        }
        if (m_cursor != XCB_CURSOR_NONE) {
            xcb_free_cursor(m_connection, m_cursor);
            m_cursor = XCB_CURSOR_NONE;
        }
        xcb_flush(m_connection);
    }

    if (m_cursorContext) {
        xcb_cursor_context_free(m_cursorContext);
        m_cursorContext = nullptr;
    }

    m_active = false;
    m_connection = nullptr;
    m_root = XCB_NONE;
}

void TargetSelector::finishSelected(const Target& target)
{
    if (m_terminalSignalEmitted) {
        return;
    }
    m_terminalSignalEmitted = true;
    cleanupX11Resources();
    Q_EMIT targetSelected(target);
}

void TargetSelector::finishCanceled(const QString& reason)
{
    if (m_terminalSignalEmitted) {
        return;
    }
    m_terminalSignalEmitted = true;
    cleanupX11Resources();
    Q_EMIT selectionCanceled(reason);
}

bool TargetSelector::handleButtonPress(const xcb_button_press_event_t* event)
{
    if (!event) {
        return false;
    }

    if (isWheelButton(event->detail)) {
        rearmPointer(event->time);
        return true;
    }

    if (event->detail == XCB_BUTTON_INDEX_2 || event->detail == XCB_BUTTON_INDEX_3) {
        finishCanceled(u"Target selection canceled"_s);
        return true;
    }

    if (event->detail != XCB_BUTTON_INDEX_1) {
        rearmPointer(event->time);
        return true;
    }

    const QPoint nativePosition(event->root_x, event->root_y);
    const auto target = m_mode == TargetSelectionMode::Screen
        ? screenTargetAt(nativePosition)
        : windowTargetAt(event->child, nativePosition);
    if (target.geometry.isEmpty()) {
        finishCanceled(u"Could not resolve the selected X11 target"_s);
        return true;
    }

    finishSelected(target);
    return true;
}

bool TargetSelector::handleButtonRelease(const xcb_button_release_event_t* event)
{
    if (!event) {
        return false;
    }

    rearmPointer(event->time);
    return true;
}

bool TargetSelector::handleKeyPress(const xcb_key_press_event_t* event)
{
    if (!event) {
        return false;
    }

    if (isEscapeKey(event->detail)) {
        finishCanceled(u"Target selection canceled"_s);
        return true;
    }

    return false;
}

void TargetSelector::rearmPointer(xcb_timestamp_t time)
{
    if (!m_connection) {
        return;
    }

    xcb_allow_events(m_connection, XCB_ALLOW_SYNC_POINTER, time);
    xcb_flush(m_connection);
}

Target TargetSelector::screenTargetAt(const QPoint& nativePosition) const
{
    Target target;
    target.mode = TargetSelectionMode::Screen;
    target.screen = screenAtNativePosition(nativePosition);
    target.geometry = nativeGeometryForScreen(target.screen);
    target.title = target.screen ? target.screen->name() : QString();
    return target;
}

Target TargetSelector::windowTargetAt(xcb_window_t clickedWindow, const QPoint& nativePosition) const
{
    Target target;
    target.mode = TargetSelectionMode::Window;

    if (clickedWindow == XCB_NONE || clickedWindow == m_root) {
        const auto pointerCookie = xcb_query_pointer_unchecked(m_connection, m_root);
        ReplyPtr pointerReplyPtr(xcb_query_pointer_reply(m_connection, pointerCookie, nullptr));
        auto pointerReply = pointerReplyPtr.get<xcb_query_pointer_reply_t>();
        clickedWindow = pointerReply ? pointerReply->child : XCB_NONE;
    }

    Q_UNUSED(nativePosition)
    const auto client = topLevelClientForWindow(m_connection, m_root, clickedWindow);
    if (client == XCB_NONE) {
        return target;
    }

    target.windowId = client;
    target.geometry = rootNativeGeometryForWindow(m_connection, m_root, client);
    target.title = titleForWindow(m_connection, client);
    return target;
}

bool TargetSelector::isEscapeKey(xcb_keycode_t keycode) const
{
    if (!m_connection) {
        return false;
    }

    const auto setup = xcb_get_setup(m_connection);
    if (!setup || keycode < setup->min_keycode || keycode > setup->max_keycode) {
        return false;
    }

    const auto cookie = xcb_get_keyboard_mapping_unchecked(m_connection, keycode, 1);
    ReplyPtr replyPtr(xcb_get_keyboard_mapping_reply(m_connection, cookie, nullptr));
    auto reply = replyPtr.get<xcb_get_keyboard_mapping_reply_t>();
    if (!reply) {
        return false;
    }

    const auto keysyms = xcb_get_keyboard_mapping_keysyms(reply);
    for (int i = 0; i < reply->keysyms_per_keycode; ++i) {
        if (keysyms[i] == escapeKeysym) {
            return true;
        }
    }
    return false;
}

} // namespace X11

#include "moc_X11TargetSelector.cpp"
