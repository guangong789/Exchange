#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "exchange/matching/event.hpp"

namespace exchange {
    class EventCollector {
    public:
        void reserve(std::size_t capacity) {
            events_.reserve(capacity);
        }

        void publish(Event event) {
            events_.push_back(std::move(event));
        }

        [[nodiscard]] const std::vector<Event>& events() const noexcept {
            return events_;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return events_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return events_.empty();
        }

        void clear() noexcept {
            events_.clear();
        }

    private:
        std::vector<Event> events_;
    };
}  // namespace exchange
