// LAST LIGHT - Acoustic Intelligence System (AIS)
// Server-side intake for acoustic events: authority check, validation,
// deduplication, per-source rate limiting and a bounded priority queue.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticEvent.h"

namespace ais {

enum class EventOrigin : std::uint8_t {
    ServerAuthoritative,  // produced by server gameplay code
    ClientClaim,          // anything that arrived from a client connection
};

enum class SubmitResult : std::uint8_t {
    Accepted = 0,
    AcceptedEvictedLower,      // queue full; a lower-priority event was dropped
    MergedDuplicate,           // same source/category/place within the dedup window
    RejectedNotAuthoritative,  // client origin or non-server world
    RejectedInvalid,           // NaN, out of bounds, stale or future tick, bad ranges
    RejectedRateLimited,       // source exceeded its token bucket or tick budget
    RejectedQueueFull,         // queue full of higher-priority events
};
inline constexpr std::size_t kSubmitResultCount = 7;
const char* ToString(SubmitResult r);
inline constexpr bool IsAccepted(SubmitResult r) {
    return r == SubmitResult::Accepted || r == SubmitResult::AcceptedEvictedLower;
}

struct BusStats {
    std::array<std::uint64_t, kSubmitResultCount> results{};
    std::uint64_t evicted = 0;
    std::uint64_t expired = 0;
    std::uint64_t drained = 0;
    std::uint64_t requeued = 0;
    std::uint64_t Count(SubmitResult r) const { return results[static_cast<std::size_t>(r)]; }
};

class AcousticEventBus {
public:
    explicit AcousticEventBus(const AisConfig& config);

    // `event.id` is ignored and assigned here (deterministic, monotonic).
    SubmitResult Submit(const AcousticEvent& event, EventOrigin origin, Tick now,
                        EventId* outId = nullptr);

    // Moves up to maxDrainPerTick events into `out`, highest priority first
    // (ties by id). Events older than maxEventAgeTicks are discarded.
    std::size_t Drain(Tick now, std::vector<AcousticEvent>& out);

    // Puts back events the world could not afford to process this tick. Skips
    // rate limiting/dedup; still bounded by capacity.
    void Requeue(const AcousticEvent& event);

    std::size_t QueueSize() const { return queue_.size(); }
    const BusStats& Stats() const { return stats_; }
    double PriorityOf(const AcousticEvent& event) const;

private:
    struct DedupEntry {
        EntityId sourceId = kNoEntity;
        StimulusCategory category = StimulusCategory::Environmental;
        Vec3 position;
        Tick tick = 0;
        EventId id = 0;
    };
    struct Bucket {
        std::uint64_t key = 0;
        double tokens = 0.0;
        Tick lastTick = 0;
    };

    bool Validate(const AcousticEvent& event, Tick now) const;
    bool TryMerge(const AcousticEvent& event, Tick now);
    bool ConsumeToken(const AcousticEvent& event, Tick now);
    void RememberForDedup(const AcousticEvent& event);
    SubmitResult Record(SubmitResult r);

    const AisConfig& config_;
    std::vector<AcousticEvent> queue_;
    std::vector<DedupEntry> dedupRing_;
    std::size_t dedupHead_ = 0;
    std::vector<Bucket> buckets_;
    EventId nextId_ = 1;
    Tick acceptedTick_ = 0;
    std::size_t acceptedThisTick_ = 0;
    BusStats stats_;
};

}  // namespace ais
