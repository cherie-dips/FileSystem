#pragma once

// flusher.h — the background writer thread (stage 9).
//
// Operations leave dirty blocks behind; this thread commits the running
// transaction now and then and writes dirty blocks back, so that eviction
// rarely has to stop and write one first. It is OSTEP 30.2's producer and
// consumer, with the wait written as a predicate loop.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace vfs {

    class Flusher {
        public:
            // work() runs on the flusher's thread every period, and soon after
            // kick(). Nothing runs until start().
            Flusher(std::chrono::milliseconds period, std::function<void()> work);
            ~Flusher();  // stop()

            Flusher(const Flusher&) = delete;
            Flusher& operator=(const Flusher&) = delete;

            void start();
            void kick();
            // Lets a work() in progress finish, then joins the thread.
            void stop();

        private:
            void run();

            const std::chrono::milliseconds period_;
            const std::function<void()> work_;
            std::mutex m_;
            std::condition_variable cv_;
            bool stop_ = false;
            bool kicked_ = false;
            std::thread thread_;
    };

} // namespace vfs
