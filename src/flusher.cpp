#include "vfs/flusher.h"

#include <utility>

namespace vfs {

    Flusher::Flusher(std::chrono::milliseconds period, std::function<void()> work)
        : period_(period), work_(std::move(work)) {
    }

    Flusher::~Flusher() {
        stop();
    }

    void Flusher::start() {
        thread_ = std::thread([this] { run(); });
    }

    void Flusher::kick() {
        {
            std::lock_guard<std::mutex> lk(m_);
            kicked_ = true;
        }
        cv_.notify_one();
    }

    void Flusher::stop() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    void Flusher::run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            // The predicate is OSTEP 30's while loop: being woken proves
            // nothing, so the condition is checked again every time.
            cv_.wait_for(lk, period_, [this] { return stop_ || kicked_; });
            if (stop_)
                break;
            kicked_ = false;

            lk.unlock();
            try {
                work_();
            } catch (...) {
                // A failing disk fails the next operation that touches it,
                // which can report it. This thread has no one to tell.
            }
            lk.lock();
        }
    }

} // namespace vfs
