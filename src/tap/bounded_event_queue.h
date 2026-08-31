#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace lvt {

template <typename T, size_t Capacity>
class BoundedEventQueue {
public:
    struct DrainResult {
        bool snapshotRequired = false;
        std::vector<T> events;
    };

    void push(T event) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_overflowed)
            return;
        if (m_events.size() >= Capacity) {
            m_events.clear();
            m_overflowed = true;
            return;
        }
        m_events.push_back(std::move(event));
    }

    DrainResult drain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        DrainResult result;
        result.snapshotRequired = m_overflowed;
        result.events.reserve(m_events.size());
        while (!m_events.empty()) {
            result.events.push_back(std::move(m_events.front()));
            m_events.pop_front();
        }
        m_overflowed = false;
        return result;
    }

private:
    std::mutex m_mutex;
    std::deque<T> m_events;
    bool m_overflowed = false;
};

} // namespace lvt
