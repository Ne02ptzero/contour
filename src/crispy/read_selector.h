// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <initializer_list>
#include <optional>
#include <vector>

#if !defined(_WIN32)
    #include <sys/select.h>

    #include <unistd.h>
#endif

#if defined(__linux__)
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
#endif

namespace crispy
{

/// Implements waiting for a set of file descriptors to become readable.
///
class posix_read_selector
{
  public:
    static posix_read_selector create(std::initializer_list<int> fds)
    {
        auto selector = posix_read_selector {};
        for (auto const fd: fds)
            selector.want_read(fd);
        return selector;
    }

    void want_read(int fd) noexcept
    {
        FD_SET(fd, &_reader);
        _fds.push_back(fd);
        std::sort(_fds.begin(), _fds.end());
    }

    void cancel_read(int fd) noexcept
    {
        FD_CLR(fd, &_reader);
        _fds.erase(std::remove(_fds.begin(), _fds.end(), fd), _fds.end());
    }

    void wakeup() noexcept
    {
        if (_breakPipe[1] != -1)
            write(_breakPipe[1], "x", 1);
    }

    std::optional<int> wait_one(std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept
    {
        assert(!_fds.empty());

        if (auto const fd = try_pop_pending(); fd.has_value())
            return fd;

        auto tv = std::unique_ptr<timeval>();
        if (timeout.has_value())
        {
            tv = std::make_unique<timeval>(
                timeval { .tv_sec = timeout->count() / 1000,
                          .tv_usec = static_cast<int>((timeout->count() % 1000) * 1000) });
        }

        auto const result = ::select(_fds.back() + 1, &_reader, &_writer, &_except, tv.get());

        if (result <= 0)
            return std::nullopt;

        if (FD_ISSET(_breakPipe[0], &_reader))
        {
            // Drain the pipe.
            char buf[256];
            while (read(_breakPipe[0], buf, sizeof(buf)) > 0)
                ;
            return std::nullopt;
        }

        for (int fd: _fds)
            if (FD_ISSET(fd, &_reader))
                _pending.push_back(fd);

        return try_pop_pending();
    }

  private:
    std::optional<int> try_pop_pending() noexcept
    {
        if (_pending.empty())
        {
            errno = EAGAIN;
            return std::nullopt;
        }

        auto const fd = _pending.front();
        _pending.pop_front();
        return fd;
    }

  private:
    fd_set _reader {};
    fd_set _writer {};
    fd_set _except {};
    std::vector<int> _fds;
    std::deque<int> _pending;
    int _breakPipe[2] { -1, -1 };
};

// {{{ epoll_read_selector, implements waiting for a set of file descriptors to become readable.
#if defined(__linux__)

/// Implements waiting for a set of file descriptors to become readable.
///
class epoll_read_selector
{
  public:
    epoll_read_selector();
    ~epoll_read_selector();

    void want_read(int fd) noexcept;
    void cancel_read(int fd) noexcept;
    void wakeup() const noexcept;
    std::optional<int> wait_one(std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept;

  private:
    std::optional<int> try_pop_pending() noexcept;

  private:
    int _epollFd = -1;
    int _eventFd = -1;
    std::deque<int> _pending;
};

inline epoll_read_selector::epoll_read_selector()
{
    _epollFd = epoll_create1(EPOLL_CLOEXEC);
    Require(_epollFd != -1);

    _eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    Require(_eventFd != -1);

    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = _eventFd;
    epoll_ctl(_epollFd, EPOLL_CTL_ADD, _eventFd, &event);
}

inline epoll_read_selector::~epoll_read_selector()
{
    close(_epollFd);
    close(_eventFd);
}

// NOLINTNEXTLINE(readability-make-member-function-const)
inline void epoll_read_selector::want_read(int fd) noexcept
{
    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &event);
}

// NOLINTNEXTLINE(readability-make-member-function-const)
inline void epoll_read_selector::cancel_read(int fd) noexcept
{
    auto event = epoll_event {};
    event.events = EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(_epollFd, EPOLL_CTL_DEL, fd, &event);
}

inline void epoll_read_selector::wakeup() const noexcept
{
    auto const value = eventfd_t { 1 };
    write(_eventFd, &value, sizeof(value));
}

inline std::optional<int> epoll_read_selector::try_pop_pending() noexcept
{
    if (_pending.empty())
        return std::nullopt;

    auto const fd = _pending.front();
    _pending.pop_front();
    return fd;
}

inline std::optional<int> epoll_read_selector::wait_one(
    std::optional<std::chrono::milliseconds> timeout) noexcept
{
    if (auto const fd = try_pop_pending(); fd.has_value())
        return fd;

    auto events = std::array<epoll_event, 64> { {} };
    for (;;)
    {
        auto const result = epoll_wait(_epollFd,
                                       events.data(),
                                       events.size(),
                                       timeout.has_value() ? static_cast<int>(timeout.value().count()) : -1);
        if (result == 0)
        {
            errno = EAGAIN;
            return std::nullopt;
        }

        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return std::nullopt;
        }

        bool piped = false;
        for (size_t i = 0; i < static_cast<size_t>(result); ++i)
        {
            if (events[i].data.fd == _eventFd)
            {
                eventfd_t dummy {};
                piped = ::read(_eventFd, &dummy, sizeof(dummy)) > 0;
            }
            else
                _pending.push_back(events[i].data.fd);
        }

        if (auto fd = try_pop_pending(); fd.has_value())
            return fd;

        errno = piped ? EINTR : EAGAIN;
        return std::nullopt;
    }
}

#endif // }}}

/// Implements waiting for a set of file descriptors to become readable.
#if defined(__linux__)
using read_selector = epoll_read_selector;
#else
using read_selector = posix_read_selector;
#endif

} // namespace crispy
