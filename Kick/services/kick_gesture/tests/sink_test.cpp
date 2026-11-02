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

#include "test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>

using namespace kickgest;

namespace
{
    int listenOn(const std::string& path)
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
        ::unlink(path.c_str());
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        EXPECT_EQ(::listen(fd, 4), 0);
        return fd;
    }

    std::string readAvailable(int fd)
    {
        std::string out;
        char buf[512];
        for (int i = 0; i < 50; ++i)
        {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0)
                out.append(buf, static_cast<size_t>(n));
            else if (!out.empty())
                break;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return out;
    }
}

TEST(UnixSocketSink, DeliversNewlineDelimitedLines)
{
    kicktest::TempDir dir;
    const std::string path = dir.file("ev.sock");
    const int srv = listenOn(path);
    UnixSocketSink sink(path, 10);
    sink.send("{\"a\":1}");
    sink.send("{\"b\":2}");
    const int conn = ::accept(srv, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    EXPECT_EQ(readAvailable(conn), "{\"a\":1}\n{\"b\":2}\n");
    EXPECT_TRUE(sink.connected());
    EXPECT_EQ(sink.dropped(), 0u);
    ::close(conn);
    ::close(srv);
    ::unlink(path.c_str());
}

TEST(UnixSocketSink, DropsWhileThereIsNoListenerAndConnectsLater)
{
    kicktest::TempDir dir;
    const std::string path = dir.file("later.sock");
    UnixSocketSink sink(path, 10);
    sink.send("lost1");
    sink.send("lost2");
    EXPECT_EQ(sink.dropped(), 2u);
    EXPECT_FALSE(sink.connected());

    const int srv = listenOn(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // past the reconnect delay
    sink.send("kept");
    const int conn = ::accept(srv, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    EXPECT_EQ(readAvailable(conn), "kept\n");
    ::close(conn);
    ::close(srv);
    ::unlink(path.c_str());
}

TEST(UnixSocketSink, ReconnectsAfterTheListenerRestarts)
{
    kicktest::TempDir dir;
    const std::string path = dir.file("restart.sock");
    int srv = listenOn(path);
    UnixSocketSink sink(path, 10);
    sink.send("one");
    int conn = ::accept(srv, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    EXPECT_EQ(readAvailable(conn), "one\n");
    ::close(conn);
    ::close(srv);
    ::unlink(path.c_str());

    // The peer is gone: the write may succeed into the buffer once, then fails. Keep sending until the sink notices.
    for (int i = 0; i < 20 && sink.connected(); ++i)
    {
        sink.send("x");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(sink.connected());

    srv = listenOn(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    sink.send("two");
    conn = ::accept(srv, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    EXPECT_EQ(readAvailable(conn), "two\n");
    ::close(conn);
    ::close(srv);
    ::unlink(path.c_str());
}

TEST(UnixSocketSink, NeverBlocksOnAReaderThatStopsReading)
{
    kicktest::TempDir dir;
    const std::string path = dir.file("slow.sock");
    const int srv = listenOn(path);
    UnixSocketSink sink(path, 10);
    sink.send("first");
    const int conn = ::accept(srv, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    // Nobody reads. Push far more than a socket buffer holds; every call must return at once.
    const std::string big(2000, 'x');
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5000; ++i) sink.send(big);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 2000);
    EXPECT_GT(sink.dropped(), 0u);
    ::close(conn);
    ::close(srv);
    ::unlink(path.c_str());
}

TEST(UnixSocketSink, OverlongPathIsHandled)
{
    UnixSocketSink sink(std::string(300, 'a'), 10);
    sink.send("x");
    EXPECT_FALSE(sink.connected());
    EXPECT_EQ(sink.dropped(), 1u);
}

TEST(FileSink, AppendsAndReportsOpenFailure)
{
    kicktest::TempDir dir;
    FileSink s;
    std::string err;
    ASSERT_TRUE(s.open(dir.file("e.jsonl"), err)) << err;
    s.send("{\"x\":1}");
    s.send("{\"x\":2}");
    std::ifstream in(dir.file("e.jsonl"));
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str(), "{\"x\":1}\n{\"x\":2}\n");

    FileSink bad;
    EXPECT_FALSE(bad.open("/nonexistent-dir/e.jsonl", err));
    EXPECT_FALSE(err.empty());
}
