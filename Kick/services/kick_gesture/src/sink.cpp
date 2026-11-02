/*
 * Copyright (c) 2024 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * Kick is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Kick is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Kick. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "kick_gesture/sink.h"

#include "kick_camera/types.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace kickgest
{
    void StdoutSink::send(const std::string& line)
    {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    bool FileSink::open(const std::string& path, std::string& error)
    {
        out_.open(path, std::ios::app);
        if (!out_)
        {
            error = "cannot open " + path + " for writing: " + std::strerror(errno);
            return false;
        }
        return true;
    }

    void FileSink::send(const std::string& line)
    {
        out_ << line << '\n';
        out_.flush();
    }

    UnixSocketSink::~UnixSocketSink() { disconnect(); }

    void UnixSocketSink::disconnect()
    {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool UnixSocketSink::tryConnect()
    {
        const uint64_t now = kickcam::monotonicNs() / 1000000ull;
        if (attempted_ && now - lastAttemptMs_ < reconnectMs_) return false;
        attempted_ = true;
        lastAttemptMs_ = now;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) return false;
        std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        // A Unix connect to a listening socket completes at once; to a full backlog it would block, so connect first
        // and only then switch to non-blocking mode for the writes.
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return false;
        }
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fd_ = fd;
        return true;
    }

    void UnixSocketSink::send(const std::string& line)
    {
        if (fd_ < 0 && !tryConnect())
        {
            ++dropped_;
            return;
        }
        const std::string data = line + "\n";
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = ::send(fd_, data.data(), data.size(), flags);
        if (n == static_cast<ssize_t>(data.size())) return;
        ++dropped_;
        if (n >= 0)
        {
            // A partial line would corrupt the stream for the reader. Close so the next line starts a new connection.
            disconnect();
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    }
}
