// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/SignalRelay.h"

#include <algorithm>

namespace ais {

bool LocalSignalRelay::Publish(const AcousticReport& report) {
    for (LastSend& s : lastSend_) {
        if (s.sender != report.senderId) continue;
        if (report.sentTick < s.tick + config_.reportMinIntervalTicks) {
            ++dropped_;
            return false;
        }
        s.tick = report.sentTick;
        break;
    }
    if (std::none_of(lastSend_.begin(), lastSend_.end(), [&](const LastSend& s) { return s.sender == report.senderId; })) {
        lastSend_.push_back({report.senderId, report.sentTick});
    }
    if (inFlight_.size() >= config_.maxReportsInFlight) {
        ++dropped_;
        return false;
    }
    inFlight_.push_back(report);
    return true;
}

void LocalSignalRelay::Deliver(Tick now, const std::vector<RelayReceiver>& receivers, std::vector<DeliveredReport>& out) {
    const double range2 = config_.reportRangeCm * config_.reportRangeCm;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < inFlight_.size(); ++i) {
        const AcousticReport& r = inFlight_[i];
        if (r.sentTick + config_.reportLatencyTicks > now) {
            inFlight_[kept++] = r;
            continue;
        }
        for (const RelayReceiver& rx : receivers) {
            if (!rx.receives || rx.id == r.senderId) continue;
            if (DistanceSq(rx.position, r.senderPosition) > range2) continue;
            out.emplace_back(rx.id, r);
        }
    }
    inFlight_.resize(kept);
}

}  // namespace ais
