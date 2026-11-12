// LAST LIGHT - Acoustic Intelligence System (AIS)
// Port to the robot signal network. In Unreal this is an adapter over the
// project's LLSignalNetwork (API not verified - see docs). The standalone
// LocalSignalRelay models range, latency and bandwidth limits.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/RobotAcousticBrain.h"

namespace ais {

struct RelayReceiver {
    EntityId id = kNoEntity;
    Vec3 position;
    bool receives = true;
};

using DeliveredReport = std::pair<EntityId, AcousticReport>;

class ISignalNetworkPort {
public:
    virtual ~ISignalNetworkPort() = default;
    // Returns false if the network dropped the report (rate/bandwidth/range).
    virtual bool Publish(const AcousticReport& report) = 0;
    // Appends (receiver, report) pairs due at `now`. Receivers are sorted by id.
    virtual void Deliver(Tick now, const std::vector<RelayReceiver>& receivers,
                         std::vector<DeliveredReport>& out) = 0;
};

class LocalSignalRelay final : public ISignalNetworkPort {
public:
    explicit LocalSignalRelay(const WorldConfig& config) : config_(config) {}

    bool Publish(const AcousticReport& report) override;
    void Deliver(Tick now, const std::vector<RelayReceiver>& receivers,
                 std::vector<DeliveredReport>& out) override;

    std::size_t InFlight() const { return inFlight_.size(); }
    std::size_t Dropped() const { return dropped_; }

private:
    struct LastSend {
        EntityId sender;
        Tick tick;
    };
    const WorldConfig& config_;
    std::vector<AcousticReport> inFlight_;
    std::vector<LastSend> lastSend_;
    std::size_t dropped_ = 0;
};

}  // namespace ais
