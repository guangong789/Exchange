#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace exchange {
    template <typename T>
    class BoundedQueue {
    public:
        explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
            if (capacity_ == 0) {
                throw std::invalid_argument("BoundedQueue capacity must be positive");
            }
        }

        BoundedQueue(const BoundedQueue&) = delete;
        BoundedQueue& operator=(const BoundedQueue&) = delete;
        BoundedQueue(BoundedQueue&&) = delete;
        BoundedQueue& operator=(BoundedQueue&&) = delete;

        bool try_push(T value) {
            {
                std::lock_guard lock(mutex_);
                if (closed_ || queue_.size() == capacity_) {
                    return false;
                }
                queue_.push_back(std::move(value));
            }
            not_empty_.notify_one();
            return true;
        }

        bool wait_push(T value) {
            {
                std::unique_lock lock(mutex_);
                not_full_.wait(
                    lock,
                    [this] { return closed_ || queue_.size() < capacity_; });
                if (closed_) {
                    return false;
                }
                queue_.push_back(std::move(value));
            }
            not_empty_.notify_one();
            return true;
        }

        [[nodiscard]] std::optional<T> try_pop() {
            std::optional<T> value;
            {
                std::lock_guard lock(mutex_);
                if (queue_.empty()) {
                    return std::nullopt;
                }
                value.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }
            not_full_.notify_one();
            return value;
        }

        [[nodiscard]] std::optional<T> wait_pop() {
            std::optional<T> value;
            {
                std::unique_lock lock(mutex_);
                not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return std::nullopt;
                }
                value.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }
            not_full_.notify_one();
            return value;
        }

        void close_and_discard() {
            {
                std::lock_guard lock(mutex_);
                if (closed_) {
                    return;
                }
                closed_ = true;
                queue_.clear();
            }
            not_empty_.notify_all();
            not_full_.notify_all();
        }

    private:
        const std::size_t capacity_;
        std::deque<T> queue_;
        std::mutex mutex_;
        std::condition_variable not_empty_;
        std::condition_variable not_full_;
        bool closed_{};
    };
}  // namespace exchange
