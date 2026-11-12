// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/AcousticEventBus.h"

#include <algorithm>

namespace ais {

const char* ToString(SubmitResult r) {
    switch (r) {
        case SubmitResult::Accepted: return "Accepted";
        case SubmitResult::AcceptedEvictedLower: return "AcceptedEvictedLower";
        case SubmitResult::MergedDuplicate: return "MergedDuplicate";
        case SubmitResult::RejectedNotAuthoritative: return "RejectedNotAuthoritative";
        case SubmitResult::RejectedInvalid: return "RejectedInvalid";
        case SubmitResult::RejectedRateLimited: return "RejectedRateLimited";
        case SubmitResult::RejectedQueueFull: return "RejectedQueueFull";
    }
    return "Unknown";
}

AcousticEventBus::AcousticEventBus(const AisConfig& config) : config_(config) {
    queue_.reserve(config.bus.queueCapacity);
    dedupRing_.reserve(config.bus.dedupHistory);
    buckets_.reserve(config.bus.maxTrackedSources);
}

SubmitResult AcousticEventBus::Record(SubmitResult r) {
    ++stats_.results[static_cast<std::size_t>(r)];
    return r;
}

double AcousticEventBus::PriorityOf(const AcousticEvent& e) const {
    const std::size_t c = Index(e.apparentCategory);
    const double bonus = c < kCategoryCount ? config_.categories[c].queuePriorityDb : 0.0;
    return e.loudnessDb + bonus;
}

bool AcousticEventBus::Validate(const AcousticEvent& e, Tick now) const {
    const BusConfig& b = config_.bus;
    if (Index(e.category) >= kCategoryCount || Index(e.apparentCategory) >= kCategoryCount) return false;
    if (Index(e.surface) >= kSurfaceCount) return false;
    if (!e.position.IsFinite()) return false;
    if (e.position.x < b.worldMin.x || e.position.y < b.worldMin.y || e.position.z < b.worldMin.z ||
        e.position.x > b.worldMax.x || e.position.y > b.worldMax.y || e.position.z > b.worldMax.z) {
        return false;
    }
    if (!std::isfinite(e.loudnessDb) || e.loudnessDb < b.minLoudnessDb || e.loudnessDb > b.maxLoudnessDb) return false;
    for (double s : e.spectrumDb) {
        if (!std::isfinite(s) || s < -60.0 || s > 30.0) return false;
    }
    if (!(e.artificiality >= 0.0 && e.artificiality <= 1.0)) return false;
    if (e.durationTicks < 1 || e.durationTicks > b.maxDurationTicks) return false;
    if (e.emitTick > now + b.maxFutureTicks) return false;
    if (now > e.emitTick && now - e.emitTick > b.maxEventAgeTicks) return false;
    return true;
}

bool AcousticEventBus::TryMerge(const AcousticEvent& e, Tick now) {
    const BusConfig& b = config_.bus;
    const double r2 = b.dedupRadiusCm * b.dedupRadiusCm;
    for (const DedupEntry& d : dedupRing_) {
        if (d.id == 0 || d.sourceId != e.sourceId || d.category != e.apparentCategory) continue;  // 0 = evicted unheard
        const Tick age = now >= d.tick ? now - d.tick : 0;
        if (age > b.dedupWindowTicks) continue;
        if (DistanceSq(d.position, e.position) > r2) continue;
        // Duplicate report of the same physical sound: keep the louder one if
        // it has not been processed yet, otherwise drop.
        for (AcousticEvent& q : queue_) {
            if (q.id == d.id) {
                q.loudnessDb = std::max(q.loudnessDb, e.loudnessDb);
                break;
            }
        }
        return true;
    }
    return false;
}

bool AcousticEventBus::ConsumeToken(const AcousticEvent& e, Tick now) {
    const BusConfig& b = config_.bus;
    // Anonymous events share one bucket per category so they cannot bypass limits.
    const std::uint64_t key = e.sourceId != kNoEntity
                                  ? static_cast<std::uint64_t>(e.sourceId)
                                  : (0xA000000000000000ull | static_cast<std::uint64_t>(Index(e.apparentCategory)));
    Bucket* bucket = nullptr;
    for (Bucket& k : buckets_) {
        if (k.key == key) {
            bucket = &k;
            break;
        }
    }
    if (bucket == nullptr) {
        if (buckets_.size() < b.maxTrackedSources) {
            buckets_.push_back(Bucket{key, b.sourceBurst, now});
            bucket = &buckets_.back();
        } else {
            // Evict the least recently used bucket (ties: lowest key).
            auto lru = std::min_element(buckets_.begin(), buckets_.end(), [](const Bucket& x, const Bucket& y) {
                return x.lastTick != y.lastTick ? x.lastTick < y.lastTick : x.key < y.key;
            });
            *lru = Bucket{key, b.sourceBurst, now};
            bucket = &*lru;
        }
    }
    if (now > bucket->lastTick) {
        bucket->tokens = std::min(b.sourceBurst,
                                  bucket->tokens + b.sourceRefillPerTick * static_cast<double>(now - bucket->lastTick));
        bucket->lastTick = now;
    }
    if (bucket->tokens < 1.0) return false;
    bucket->tokens -= 1.0;
    return true;
}

void AcousticEventBus::RememberForDedup(const AcousticEvent& e) {
    const DedupEntry entry{e.sourceId, e.apparentCategory, e.position, e.emitTick, e.id};
    if (dedupRing_.size() < config_.bus.dedupHistory) {
        dedupRing_.push_back(entry);
    } else {
        dedupRing_[dedupHead_] = entry;
        dedupHead_ = (dedupHead_ + 1) % dedupRing_.size();
    }
}

SubmitResult AcousticEventBus::Submit(const AcousticEvent& event, EventOrigin origin, Tick now, EventId* outId) {
    if (outId != nullptr) *outId = 0;
    if (origin != EventOrigin::ServerAuthoritative) return Record(SubmitResult::RejectedNotAuthoritative);
    if (!Validate(event, now)) return Record(SubmitResult::RejectedInvalid);
    if (TryMerge(event, now)) return Record(SubmitResult::MergedDuplicate);

    if (acceptedTick_ != now) {
        acceptedTick_ = now;
        acceptedThisTick_ = 0;
    }
    if (acceptedThisTick_ >= config_.bus.maxAcceptedPerTick) return Record(SubmitResult::RejectedRateLimited);
    if (!ConsumeToken(event, now)) return Record(SubmitResult::RejectedRateLimited);

    AcousticEvent e = event;
    SubmitResult result = SubmitResult::Accepted;
    if (queue_.size() >= config_.bus.queueCapacity) {
        if (queue_.empty()) return Record(SubmitResult::RejectedQueueFull);  // capacity 0: nothing to evict
        // Evict the lowest-priority queued event if the newcomer outranks it.
        auto lowest = std::min_element(queue_.begin(), queue_.end(), [this](const AcousticEvent& a, const AcousticEvent& b) {
            const double pa = PriorityOf(a);
            const double pb = PriorityOf(b);
            return pa != pb ? pa < pb : a.id > b.id;  // newest of equal priority goes first
        });
        if (PriorityOf(e) <= PriorityOf(*lowest)) return Record(SubmitResult::RejectedQueueFull);
        // The evicted sound was never heard: a repeat of it must not be merged away.
        for (DedupEntry& d : dedupRing_) {
            if (d.id == lowest->id) d.id = 0;
        }
        queue_.erase(lowest);
        ++stats_.evicted;
        result = SubmitResult::AcceptedEvictedLower;
    }
    e.id = nextId_++;
    queue_.push_back(e);
    ++acceptedThisTick_;
    RememberForDedup(e);
    if (outId != nullptr) *outId = e.id;
    return Record(result);
}

std::size_t AcousticEventBus::Drain(Tick now, std::vector<AcousticEvent>& out) {
    const Tick maxAge = config_.bus.maxEventAgeTicks;
    const auto expired = [now, maxAge](const AcousticEvent& e) { return now > e.emitTick && now - e.emitTick > maxAge; };
    const std::size_t before = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), expired), queue_.end());
    stats_.expired += before - queue_.size();

    std::sort(queue_.begin(), queue_.end(), [this](const AcousticEvent& a, const AcousticEvent& b) {
        const double pa = PriorityOf(a);
        const double pb = PriorityOf(b);
        return pa != pb ? pa > pb : a.id < b.id;
    });
    const std::size_t n = std::min(queue_.size(), config_.bus.maxDrainPerTick);
    out.insert(out.end(), queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
    stats_.drained += n;
    return n;
}

void AcousticEventBus::Requeue(const AcousticEvent& event) {
    if (queue_.size() >= config_.bus.queueCapacity) {
        ++stats_.evicted;
        return;
    }
    queue_.push_back(event);
    ++stats_.requeued;
}

}  // namespace ais
