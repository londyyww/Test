// LAST LIGHT - bounded deterministic scheduler for the world simulation.
//
// A min-heap ordered by (due tick, insertion sequence), so ties always resolve
// in insertion order on every platform. Capacity is fixed; cancelled entries are
// invalidated by generation counters held by the owner and removed lazily, or
// eagerly by Compact() when the queue is full.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "llworld/WorldTypes.h"

namespace llsim {

struct SchedEntry {
    llworld::Tick due = 0;
    std::uint64_t seq = 0;
    std::uint32_t target = 0;
    std::uint16_t kind = 0;
    std::uint32_t gen = 0;
};

class BoundedScheduler {
public:
    explicit BoundedScheduler(std::size_t capacity = 1024) : cap_(capacity) { heap_.reserve(capacity); }

    // false when full; the caller may Compact() and retry.
    bool Push(llworld::Tick due, std::uint32_t target, std::uint16_t kind, std::uint32_t gen) {
        if (heap_.size() >= cap_) return false;
        heap_.push_back(SchedEntry{due, seq_++, target, kind, gen});
        std::push_heap(heap_.begin(), heap_.end(), Later);
        return true;
    }
    bool HasDue(llworld::Tick now) const { return !heap_.empty() && heap_.front().due <= now; }
    const SchedEntry& Top() const { return heap_.front(); }
    void Pop() {
        std::pop_heap(heap_.begin(), heap_.end(), Later);
        heap_.pop_back();
    }
    template <class IsLive>
    std::size_t Compact(IsLive live) {
        const std::size_t before = heap_.size();
        heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&live](const SchedEntry& e) { return !live(e); }), heap_.end());
        std::make_heap(heap_.begin(), heap_.end(), Later);
        return before - heap_.size();
    }
    void Clear() {
        heap_.clear();
        seq_ = 0;
    }
    std::size_t Size() const { return heap_.size(); }
    std::size_t Capacity() const { return cap_; }

private:
    static bool Later(const SchedEntry& a, const SchedEntry& b) { return a.due != b.due ? a.due > b.due : a.seq > b.seq; }
    std::vector<SchedEntry> heap_;
    std::size_t cap_;
    std::uint64_t seq_ = 0;
};

}  // namespace llsim
