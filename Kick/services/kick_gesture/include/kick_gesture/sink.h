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
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace kickgest
{
    // Where event lines go. send() takes one JSON object without the newline and must never block the caller for long.
    class IEventSink
    {
    public:
        virtual ~IEventSink() = default;
        virtual void send(const std::string& line) = 0;
    };

    class StdoutSink final : public IEventSink
    {
    public:
        void send(const std::string& line) override;
    };

    // Appends lines to a file and flushes after each one.
    class FileSink final : public IEventSink
    {
    public:
        // False when the file cannot be opened.
        bool open(const std::string& path, std::string& error);
        void send(const std::string& line) override;

    private:
        std::ofstream out_;
    };

    // Keeps lines in memory. For tests.
    class MemorySink final : public IEventSink
    {
    public:
        void send(const std::string& line) override
        {
            std::lock_guard<std::mutex> g(m_);
            lines_.push_back(line);
        }
        std::vector<std::string> lines() const
        {
            std::lock_guard<std::mutex> g(m_);
            return lines_;
        }

    private:
        mutable std::mutex m_;
        std::vector<std::string> lines_;
    };

    // Newline-delimited JSON over a Unix stream socket the listener (the Kick app) owns. The service is the client:
    // it connects when the listener exists, writes without blocking, and drops a line it cannot write right away
    // instead of waiting, so a stuck app can never slow gesture recognition down. Gesture events are not worth queueing:
    // a swipe that arrives a second late is wrong. A dropped line is counted.
    class UnixSocketSink final : public IEventSink
    {
    public:
        explicit UnixSocketSink(std::string path, uint32_t reconnectMs = 1000) : path_(std::move(path)), reconnectMs_(reconnectMs) {}
        ~UnixSocketSink() override;
        void send(const std::string& line) override;

        bool connected() const { return fd_ >= 0; }
        uint64_t dropped() const { return dropped_; }

    private:
        bool tryConnect();
        void disconnect();

        std::string path_;
        uint32_t reconnectMs_;
        int fd_ = -1;
        uint64_t lastAttemptMs_ = 0;
        bool attempted_ = false;
        uint64_t dropped_ = 0;
    };
}
