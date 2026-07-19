/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <memory>

#include <xcb/xcb.h>

namespace X11 {

class Connection {
public:
    Connection();
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool isValid() const;
    xcb_connection_t* handle() const;
    xcb_window_t rootWindow(int screenNumber = -1) const;
    int defaultScreenNumber() const;
    int screenCount() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class ReplyPtr {
public:
    explicit ReplyPtr(void* reply = nullptr) noexcept;
    ~ReplyPtr();
    ReplyPtr(ReplyPtr&& other) noexcept;
    ReplyPtr& operator=(ReplyPtr&& other) noexcept;
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
    void reset(void* reply = nullptr);

private:
    void* m_reply;
};

// Models
struct Monitor {
    QString name;
    QRect rect;
    bool primary = false;
};

class ScreenModel {
public:
    bool populate(Connection& conn);
    const QList<Monitor>& monitors() const
    {
        return m_monitors;
    }
    QRect monitorRectAt(const QPoint& nativePos) const;

private:
    QList<Monitor> m_monitors;
};

// Point selection callback would invoke this from the main thread.
enum class SelectionResult {
    Accepted,
    Canceled,
};

} // namespace X11
