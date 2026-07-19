/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>

namespace X11 {

class TargetSelector;

enum class TargetSelectionMode {
    Screen,
    Window,
};

struct Target {
    TargetSelectionMode mode = TargetSelectionMode::Screen;
    QRect geometry; // root-native geometry for screen/window selections
    xcb_window_t windowId = XCB_NONE;
    QString title;
    QPointer<QScreen> screen;
};

class TargetSelector : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit TargetSelector(TargetSelectionMode mode, QObject* parent = nullptr);
    ~TargetSelector() override;

    bool start();
    bool isActive() const
    {
        return m_active;
    }

    void cancel(const QString& reason = QString());

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    static xcb_connection_t* nativeConnection();
    static xcb_window_t defaultRootWindow(xcb_connection_t* connection);
    static QRect nativeGeometryForScreen(const QScreen* screen);
    static QScreen* screenAtNativePosition(const QPoint& nativePosition);
    static xcb_window_t topLevelClientForWindow(xcb_connection_t* connection, xcb_window_t root, xcb_window_t window);
    static QRect rootNativeGeometryForWindow(xcb_connection_t* connection, xcb_window_t root, xcb_window_t window);
    static QString titleForWindow(xcb_connection_t* connection, xcb_window_t window);

Q_SIGNALS:
    void targetSelected(const X11::Target& target);
    void selectionCanceled(const QString& reason);

private:
    bool setupX11Resources();
    bool grabInput();
    void cleanupX11Resources();
    void finishSelected(const Target& target);
    void finishCanceled(const QString& reason);
    bool handleButtonPress(const xcb_button_press_event_t* event);
    bool handleButtonRelease(const xcb_button_release_event_t* event);
    bool handleKeyPress(const xcb_key_press_event_t* event);
    void rearmPointer(xcb_timestamp_t time);
    Target screenTargetAt(const QPoint& nativePosition) const;
    Target windowTargetAt(xcb_window_t clickedWindow, const QPoint& nativePosition) const;
    bool isEscapeKey(xcb_keycode_t keycode) const;

    TargetSelectionMode m_mode;
    xcb_connection_t* m_connection = nullptr;
    xcb_window_t m_root = XCB_NONE;
    xcb_cursor_t m_cursor = XCB_CURSOR_NONE;
    xcb_cursor_context_t* m_cursorContext = nullptr;
    bool m_pointerGrabbed = false;
    bool m_keyboardGrabbed = false;
    bool m_filterInstalled = false;
    bool m_active = false;
    bool m_terminalSignalEmitted = false;
};

} // namespace X11

Q_DECLARE_METATYPE(X11::Target)
