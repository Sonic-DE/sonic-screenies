/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "X11Connection.h"

#include <xcb/xcb.h>

#include <QDebug>

using namespace Qt::StringLiterals;

namespace X11 {

struct Connection::Private {
    xcb_connection_t* handle = nullptr;
    int screenNumber = 0;
    int screenCount = 0;
};

Connection::Connection()
    : d(std::make_unique<Private>())
{
    d->handle = xcb_connect(nullptr, &d->screenNumber);
    if (!d->handle || xcb_connection_has_error(d->handle)) {
        if (d->handle) {
            xcb_disconnect(d->handle);
            d->handle = nullptr;
        }
        return;
    }
    const auto setup = xcb_get_setup(d->handle);
    auto iterator = xcb_setup_roots_iterator(setup);
    while (iterator.rem) {
        ++d->screenCount;
        xcb_screen_next(&iterator);
    }
}

Connection::~Connection()
{
    if (d->handle) {
        xcb_disconnect(d->handle);
        d->handle = nullptr;
    }
}

bool Connection::isValid() const
{
    return d->handle && xcb_connection_has_error(d->handle) == 0;
}

xcb_connection_t* Connection::handle() const
{
    return d->handle;
}

xcb_window_t Connection::rootWindow(int screenNumber) const
{
    if (!isValid()) {
        return XCB_NONE;
    }
    if (screenNumber < 0) {
        screenNumber = d->screenNumber;
    }
    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(d->handle));
    for (int i = 0; iterator.rem && i < screenNumber; ++i) {
        xcb_screen_next(&iterator);
    }
    return iterator.rem ? iterator.data->root : XCB_NONE;
}

int Connection::defaultScreenNumber() const
{
    return d->screenNumber;
}

int Connection::screenCount() const
{
    return d->screenCount;
}

ReplyPtr::ReplyPtr(void* reply) noexcept
    : m_reply(reply)
{
}

ReplyPtr::~ReplyPtr()
{
    reset();
}

ReplyPtr::ReplyPtr(ReplyPtr&& other) noexcept
    : m_reply(other.m_reply)
{
    other.m_reply = nullptr;
}

ReplyPtr& ReplyPtr::operator=(ReplyPtr&& other) noexcept
{
    if (this != &other) {
        reset();
        m_reply = other.m_reply;
        other.m_reply = nullptr;
    }
    return *this;
}

void ReplyPtr::reset(void* reply)
{
    if (m_reply) {
        free(m_reply);
        m_reply = nullptr;
    }
    m_reply = reply;
}

bool ScreenModel::populate(Connection& conn)
{
    m_monitors.clear();
    if (!conn.isValid()) {
        return false;
    }
    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(conn.handle()));
    for (int index = 0; iterator.rem; ++index) {
        const auto screen = iterator.data;
        Monitor monitor;
        monitor.name = QStringLiteral("X11-%1").arg(index);
        monitor.rect = QRect(0, 0, screen->width_in_pixels, screen->height_in_pixels);
        monitor.primary = index == conn.defaultScreenNumber();
        m_monitors.append(monitor);
        xcb_screen_next(&iterator);
    }
    return !m_monitors.isEmpty();
}

} // namespace X11
