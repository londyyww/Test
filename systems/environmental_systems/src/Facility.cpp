// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
#include "llenv/Facility.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace llenv {

namespace {

bool HeapLater(const TimerEntry& a, const TimerEntry& b) {
    return a.tick != b.tick ? a.tick > b.tick : a.seq > b.seq;
}

constexpr double kAlarmDefaultDb = 95.0;
constexpr double kEps = 1e-9;

}  // namespace

// ===========================================================================
// Construction
// ===========================================================================

Facility::Facility(const FacilityDef& def, AuthorityRole role, FacilityConfig config)
    : def_(def), role_(role), config_(config) {
    errors_ = ValidateFacility(def_);
    if (!errors_.empty()) return;

    const std::size_t n = def_.nodes.size();
    nodes_.resize(n);
    children_.resize(n);
    parents_.resize(n);
    networkOf_.resize(n);

    std::vector<std::string> names;
    for (std::size_t i = 0; i < n; ++i) {
        const NodeDef& d = def_.nodes[i];
        auto it = std::find(names.begin(), names.end(), d.network);
        if (it == names.end()) {
            names.push_back(d.network);
            it = names.end() - 1;
        }
        networkOf_[i] = static_cast<std::size_t>(it - names.begin());
        for (const auto& f : d.fedBy) {
            const auto p = static_cast<NodeIndex>(def_.Find(f));
            parents_[i].push_back(p);
            children_[p].push_back(static_cast<NodeIndex>(i));
        }
    }
    for (auto& c : children_) std::sort(c.begin(), c.end());
    networkNodes_.resize(names.size());
    networks_.resize(names.size());
    networkDirty_.assign(names.size(), 1);
    for (std::size_t k = 0; k < names.size(); ++k) networks_[k].name = names[k];
    for (std::size_t i = 0; i < n; ++i) networkNodes_[networkOf_[i]].push_back(static_cast<NodeIndex>(i));

    for (std::size_t i = 0; i < n; ++i) {
        const NodeDef& d = def_.nodes[i];
        NodeState& s = nodes_[i];
        s.health = d.health;
        s.destroyed = d.health <= 0.0;
        s.switchedOn = d.type == NodeType::Battery ? true : d.startsOn;
        s.gen = s.destroyed ? GeneratorState::Destroyed : GeneratorState::Off;
        s.fuel = d.fuelTicks;
        s.charge = d.batteryCapacityKwTicks * d.batteryInitialFraction;
        s.closed = d.startsClosed;
        s.fuseInstalled = d.fuseInstalled;
        s.door = d.startsOpen ? DoorState::Open : DoorState::Closed;
        s.lockCommanded = d.startsLocked;
        s.armCommanded = d.startsOn;
        if (role_ == AuthorityRole::Client) s.version = 0;  // first replication always applies
        if (d.type == NodeType::Light || d.type == NodeType::EmergencyLight) lights_.push_back(static_cast<NodeIndex>(i));
        if (d.type == NodeType::SecuritySensor) sensors_.push_back(static_cast<NodeIndex>(i));
    }
}

// ===========================================================================
// Players
// ===========================================================================

PlayerSession* Facility::FindPlayer(PlayerId id) {
    for (auto& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const PlayerSession* Facility::Player(PlayerId id) const {
    for (const auto& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool Facility::ConnectPlayer(PlayerId id, const Vec3& position, std::uint32_t permissions) {
    if (role_ != AuthorityRole::Server || !IsValid() || id == kNoPlayer || !position.IsFinite()) return false;
    const auto connected = static_cast<std::size_t>(
        std::count_if(players_.begin(), players_.end(), [](const PlayerSession& p) { return p.connected; }));
    PlayerSession* existing = FindPlayer(id);
    if (existing != nullptr && existing->connected) return false;
    if (connected >= config_.maxPlayers) return false;
    if (existing == nullptr) {
        PlayerSession s;
        s.id = id;
        s.tokens = config_.requestBurst;
        s.tokenStamp = tick_;
        auto at = std::lower_bound(players_.begin(), players_.end(), id,
                                   [](const PlayerSession& p, PlayerId v) { return p.id < v; });
        existing = &*players_.insert(at, s);
    }
    // Reconnect keeps the dedup history so replayed requests stay rejected.
    existing->connected = true;
    existing->position = position;
    existing->permissions = permissions;
    return true;
}

bool Facility::DisconnectPlayer(PlayerId id) {
    PlayerSession* p = FindPlayer(id);
    if (role_ != AuthorityRole::Server || p == nullptr || !p->connected) return false;
    p->connected = false;
    RemoveFromChannels(id);
    return true;
}

bool Facility::UpdatePlayerPosition(PlayerId id, const Vec3& position) {
    PlayerSession* p = FindPlayer(id);
    if (role_ != AuthorityRole::Server || p == nullptr || !p->connected || !position.IsFinite()) return false;
    p->position = position;
    return true;
}

// ===========================================================================
// Helpers
// ===========================================================================

NodeIndex Facility::Find(std::string_view id) const {
    const int i = def_.Find(id);
    return i < 0 ? kInvalidNode : static_cast<NodeIndex>(i);
}

bool Facility::IsDamaged(NodeIndex i) const {
    return !nodes_[i].destroyed && nodes_[i].health < def_.nodes[i].damagedThreshold;
}

bool Facility::IsLocked(NodeIndex i) const {
    const NodeState& s = nodes_[i];
    if (!IsDoorLike(def_.nodes[i].type)) return false;
    return s.lockCommanded && !s.lockBroken && (s.powered || def_.nodes[i].failMode == LockFailMode::FailSecure);
}

bool Facility::IsPassable(NodeIndex i) const {
    if (!IsDoorLike(def_.nodes[i].type)) return !BlocksNavigation(i);
    return nodes_[i].door == DoorState::Open;
}

bool Facility::BlocksNavigation(NodeIndex i) const {
    const NodeDef& d = def_.nodes[i];
    if (IsDoorLike(d.type)) return nodes_[i].door != DoorState::Open;
    if (d.type == NodeType::Machinery) return d.blocksNavWhenRunning && nodes_[i].soundActive;
    return false;
}

double Facility::IlluminationAt(const Vec3& position) const {
    double total = 0.0;
    for (NodeIndex i : lights_) {
        const NodeState& s = nodes_[i];
        if (!s.lit) continue;
        const NodeDef& d = def_.nodes[i];
        const double intensity = s.onEmergency ? d.emergencyIntensity : d.lightIntensity;
        total += intensity * std::max(0.0, 1.0 - Distance(position, d.position) / d.lightRadiusCm);
    }
    return total;
}

std::size_t Facility::ActiveAlarms() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (def_.nodes[i].type == NodeType::Alarm && nodes_[i].alarmActive && nodes_[i].powered) ++n;
    }
    return n;
}

void Facility::Changed(NodeIndex i) {
    ++nodes_[i].version;
    nodes_[i].replicationDirty = true;
}

void Facility::Emit(FacilityEventType type, NodeIndex i, std::int64_t value, std::string text) {
    if (eventsThisTick_ >= config_.maxEventsPerTick) {
        ++stats_.eventsDropped;
        return;
    }
    ++eventsThisTick_;
    ++stats_.eventsEmitted;
    if (observer_ != nullptr) observer_->OnFacilityEvent(FacilityEvent{type, i, tick_, value, std::move(text)});
}

void Facility::Sound(SoundKind kind, NodeIndex i, double loudnessDb, Tick duration) {
    if (soundsThisTick_ >= config_.maxSoundsPerTick) {
        ++stats_.soundsDropped;
        return;
    }
    ++soundsThisTick_;
    ++stats_.soundsEmitted;
    if (sound_ != nullptr) sound_->OnSound(EnvSoundEvent{kind, i, def_.nodes[i].position, loudnessDb, duration, tick_});
}

InteractionResult Facility::Record(InteractionResult r) {
    ++stats_.results[static_cast<std::size_t>(r)];
    return r;
}

// ===========================================================================
// Timers
// ===========================================================================

void Facility::Schedule(NodeIndex i, TimerKind kind, Tick delay) {
    const auto k = static_cast<std::size_t>(kind);
    NodeState& s = nodes_[i];
    ++s.timerGen[k];
    s.timerPending[k] = true;
    timers_.push_back(TimerEntry{tick_ + std::max<Tick>(1, delay), timerSeq_++, i, kind, s.timerGen[k]});
    std::push_heap(timers_.begin(), timers_.end(), HeapLater);
    // Lazily cancelled entries are compacted so spam cannot grow the heap.
    if (timers_.size() > 8 * nodes_.size() + 64) {
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [this](const TimerEntry& t) {
                                         const auto kk = static_cast<std::size_t>(t.kind);
                                         return !nodes_[t.node].timerPending[kk] || nodes_[t.node].timerGen[kk] != t.gen;
                                     }),
                      timers_.end());
        std::make_heap(timers_.begin(), timers_.end(), HeapLater);
    }
}

void Facility::Cancel(NodeIndex i, TimerKind kind) {
    const auto k = static_cast<std::size_t>(kind);
    ++nodes_[i].timerGen[k];
    nodes_[i].timerPending[k] = false;
}

void Facility::FireTimers() {
    while (!timers_.empty() && timers_.front().tick <= tick_) {
        std::pop_heap(timers_.begin(), timers_.end(), HeapLater);
        const TimerEntry t = timers_.back();
        timers_.pop_back();
        const auto k = static_cast<std::size_t>(t.kind);
        NodeState& s = nodes_[t.node];
        if (!s.timerPending[k] || s.timerGen[k] != t.gen) {
            ++stats_.staleTimersSkipped;
            continue;
        }
        s.timerPending[k] = false;
        ++stats_.timersFired;
        HandleTimer(t.node, t.kind);
    }
}

void Facility::HandleTimer(NodeIndex i, TimerKind kind) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    switch (kind) {
        case TimerKind::GeneratorStarted:
            if (s.gen == GeneratorState::Starting) SetGenerator(i, GeneratorState::Running);
            break;
        case TimerKind::FuelEmpty:
            IntegrateFuel(i);
            s.fuel = 0.0;
            s.autoStarted = false;
            SetGenerator(i, GeneratorState::Off);
            break;
        case TimerKind::OverloadTrip:
            if (s.gen == GeneratorState::Running) {
                s.autoStarted = false;
                SetGenerator(i, GeneratorState::Tripped);
                Sound(SoundKind::GeneratorFailure, i, d.noiseDb + 15.0, 3);
            }
            break;
        case TimerKind::BackupAutoStart:
            if (s.gen == GeneratorState::Off && StartGenerator(i, kNoPlayer) == InteractionResult::Applied &&
                s.gen == GeneratorState::Starting) {
                s.autoStarted = true;
            }
            break;
        case TimerKind::BatteryEmpty:
            IntegrateBattery(i);
            s.charge = 0.0;
            MarkNetworkDirty(i);
            break;
        case TimerKind::BatteryFull:
            IntegrateBattery(i);
            s.charge = d.batteryCapacityKwTicks;
            MarkNetworkDirty(i);
            break;
        case TimerKind::BreakerTrip:
            TripBreaker(i);
            break;
        case TimerKind::DoorMotionDone:
            FinishDoorMotion(i);
            break;
        case TimerKind::AlarmPulse:
            if (s.alarmActive && s.powered && !s.destroyed) {
                Sound(SoundKind::AlarmPulse, i, d.noiseDb > 0.0 ? d.noiseDb : kAlarmDefaultDb, d.alarmIntervalTicks);
                Schedule(i, TimerKind::AlarmPulse, d.alarmIntervalTicks);
            }
            break;
        case TimerKind::AlarmEnd:
            SetAlarm(i, false);
            break;
        case TimerKind::EmergencyEnd:
            s.onEmergency = false;
            UpdateLight(i);
            break;
        case TimerKind::NoisePulse: {
            bool again = false;
            if ((d.type == NodeType::Generator || d.type == NodeType::BackupGenerator) && s.gen == GeneratorState::Running) {
                if (d.noiseDb > 0.0) Sound(SoundKind::GeneratorRunning, i, d.noiseDb, d.noisePulseTicks);
                again = true;
            } else if (d.type == NodeType::Machinery && s.soundActive) {
                if (d.noiseDb > 0.0) Sound(SoundKind::MachineryRunning, i, d.noiseDb, d.noisePulseTicks);
                again = true;
            } else if (d.type == NodeType::CommArray && s.powered && s.switchedOn && !s.destroyed) {
                if (d.noiseDb > 0.0) Sound(SoundKind::CommBurst, i, d.noiseDb, 15);
                again = true;
            }
            if (again) Schedule(i, TimerKind::NoisePulse, d.noisePulseTicks);
            break;
        }
    }
}

// ===========================================================================
// Power grid
// ===========================================================================

void Facility::MarkNetworkDirty(NodeIndex i) { networkDirty_[networkOf_[i]] = 1; }

bool Facility::Conducts(NodeIndex i) const {
    const NodeState& s = nodes_[i];
    switch (def_.nodes[i].type) {
        case NodeType::Junction: return !s.destroyed;
        case NodeType::Breaker: return !s.destroyed && s.closed && !s.tripped && s.fuseInstalled;
        default: return false;
    }
}

double Facility::SourceOutput(NodeIndex i) const {
    const NodeDef& d = def_.nodes[i];
    const NodeState& s = nodes_[i];
    if (s.destroyed) return 0.0;
    if (d.type == NodeType::Generator || d.type == NodeType::BackupGenerator) {
        if (s.gen != GeneratorState::Running) return 0.0;
        return d.capacityKw * (IsDamaged(i) ? d.damagedOutputFactor : 1.0);
    }
    if (d.type == NodeType::Battery) return (s.switchedOn && s.charge > kEps) ? d.capacityKw : 0.0;
    return 0.0;
}

bool Facility::WantsPower(NodeIndex i) const {
    const NodeDef& d = def_.nodes[i];
    if (!IsConsumer(d.type) || nodes_[i].destroyed) return false;
    switch (d.type) {
        case NodeType::Light:
        case NodeType::Machinery:
        case NodeType::CommArray: return nodes_[i].switchedOn;
        default: return true;
    }
}

void Facility::IntegrateFuel(NodeIndex i) {
    NodeState& s = nodes_[i];
    if (s.gen == GeneratorState::Running && s.fuel >= 0.0 && tick_ > s.fuelStamp) {
        s.fuel = std::max(0.0, s.fuel - static_cast<double>(tick_ - s.fuelStamp));
    }
    s.fuelStamp = tick_;
}

void Facility::IntegrateBattery(NodeIndex i) {
    NodeState& s = nodes_[i];
    if (tick_ > s.chargeStamp) {
        const double cap = def_.nodes[i].batteryCapacityKwTicks;
        s.charge = std::clamp(s.charge - s.batteryRateKw * static_cast<double>(tick_ - s.chargeStamp), 0.0, cap);
    }
    s.chargeStamp = tick_;
}

void Facility::Energize(std::size_t network, bool includeBackup, std::vector<char>& out) const {
    std::vector<NodeIndex> queue;
    for (NodeIndex i : networkNodes_[network]) {
        out[i] = 0;
    }
    for (NodeIndex i : networkNodes_[network]) {
        const NodeType t = def_.nodes[i].type;
        if (!IsSource(t) || SourceOutput(i) <= 0.0) continue;
        if (!includeBackup && t != NodeType::Generator) continue;
        out[i] = 1;
        queue.push_back(i);
    }
    for (std::size_t q = 0; q < queue.size(); ++q) {
        for (NodeIndex c : children_[queue[q]]) {
            if (out[c] != 0) continue;
            if (IsConsumer(def_.nodes[c].type)) {
                out[c] = 1;
            } else if (Conducts(c)) {
                out[c] = 1;
                queue.push_back(c);
            }
        }
    }
}

void Facility::SolveDirtyNetworks() {
    for (std::size_t iter = 0; iter < config_.maxSolveIterations; ++iter) {
        bool any = false;
        for (std::size_t n = 0; n < networks_.size(); ++n) {
            if (networkDirty_[n] == 0) continue;
            networkDirty_[n] = 0;
            any = true;
            SolveNetwork(n);
        }
        if (!any) break;
    }
}

void Facility::SolveNetwork(std::size_t network) {
    ++stats_.powerSolves;
    const std::vector<NodeIndex>& members = networkNodes_[network];
    const std::size_t total = nodes_.size();

    for (NodeIndex i : members) {
        if (def_.nodes[i].type == NodeType::Battery) IntegrateBattery(i);
    }

    std::vector<char> live(total, 0);
    Energize(network, true, live);
    // Empty-but-connected batteries join their island so surplus can recharge them.
    for (NodeIndex b : members) {
        if (def_.nodes[b].type != NodeType::Battery || live[b] != 0 || nodes_[b].destroyed || !nodes_[b].switchedOn) continue;
        if (std::any_of(children_[b].begin(), children_[b].end(), [&live](NodeIndex c) { return live[c] != 0; })) live[b] = 1;
    }

    // Islands: union-find over live edges.
    std::vector<NodeIndex> uf(total);
    for (NodeIndex i : members) uf[i] = i;
    auto findRoot = [&uf](NodeIndex x) {
        while (uf[x] != x) {
            uf[x] = uf[uf[x]];
            x = uf[x];
        }
        return x;
    };
    for (NodeIndex i : members) {
        if (live[i] == 0) continue;
        for (NodeIndex c : children_[i]) {
            if (live[c] == 0) continue;
            const NodeIndex a = findRoot(i);
            const NodeIndex b = findRoot(c);
            if (a != b) uf[std::max(a, b)] = std::min(a, b);
        }
    }

    struct Island {
        NodeIndex root;
        double genSupply = 0.0;
        double demand = 0.0;
        double minTolerance = 1e9;
        std::vector<NodeIndex> gens, batteries, consumers;
    };
    std::vector<Island> islands;
    std::vector<char> newPowered(total, 0);
    std::vector<double> batteryRate(total, 0.0);
    auto islandOf = [&](NodeIndex root) -> Island& {
        for (auto& is : islands) {
            if (is.root == root) return is;
        }
        Island is;
        is.root = root;
        islands.push_back(is);
        return islands.back();
    };
    for (NodeIndex i : members) {
        if (live[i] == 0) continue;
        const NodeDef& d = def_.nodes[i];
        Island& is = islandOf(findRoot(i));
        if (d.type == NodeType::Battery) {
            is.batteries.push_back(i);
        } else if (IsSource(d.type)) {
            is.genSupply += SourceOutput(i);
            is.minTolerance = std::min(is.minTolerance, d.overloadTolerance);
            is.gens.push_back(i);
        } else if (IsConsumer(d.type) && WantsPower(i)) {
            is.consumers.push_back(i);
            is.demand += d.demandKw;
        }
    }

    NetworkStatus& status = networks_[network];
    const bool wasOverloaded = status.overloaded;
    status.supplyKw = status.demandKw = status.servedKw = 0.0;
    status.islands = islands.size();
    status.shedConsumers = 0;
    status.overloaded = false;

    for (Island& is : islands) {
        double deficit = std::max(0.0, is.demand - is.genSupply);
        double batterySupply = 0.0;
        double batteryMax = 0.0;
        for (NodeIndex b : is.batteries) {
            if (nodes_[b].charge <= kEps) continue;  // connected for charging only
            const double give = std::min(def_.nodes[b].capacityKw, deficit);
            batteryMax += def_.nodes[b].capacityKw;
            batteryRate[b] = give;
            deficit -= give;
            batterySupply += give;
        }
        // Load shedding: highest priority first, first fit.
        std::sort(is.consumers.begin(), is.consumers.end(), [this](NodeIndex a, NodeIndex b) {
            const int pa = def_.nodes[a].priority;
            const int pb = def_.nodes[b].priority;
            return pa != pb ? pa > pb : a < b;
        });
        double remaining = is.genSupply + batterySupply;
        for (NodeIndex c : is.consumers) {
            const double dem = def_.nodes[c].demandKw;
            if (dem <= remaining + kEps) {
                newPowered[c] = 1;
                remaining -= dem;
                status.servedKw += dem;
            } else {
                ++status.shedConsumers;
            }
        }
        // Surplus recharges idle batteries.
        for (NodeIndex b : is.batteries) {
            if (batteryRate[b] > 0.0) continue;
            const double want = std::min(def_.nodes[b].batteryChargeKw, remaining);
            if (want > kEps && nodes_[b].charge < def_.nodes[b].batteryCapacityKwTicks - kEps) {
                batteryRate[b] = -want;
                remaining -= want;
            }
        }
        // Generator overload: requested demand beyond what the generators can carry.
        const bool overloaded =
            !is.gens.empty() && is.demand > (is.genSupply + batteryMax) * is.minTolerance + kEps;
        for (NodeIndex g : is.gens) {
            if (overloaded && !Pending(g, TimerKind::OverloadTrip)) Schedule(g, TimerKind::OverloadTrip, def_.nodes[g].overloadTripTicks);
            if (!overloaded && Pending(g, TimerKind::OverloadTrip)) Cancel(g, TimerKind::OverloadTrip);
        }
        status.overloaded = status.overloaded || overloaded;
        status.supplyKw += is.genSupply + batteryMax;
        status.demandKw += is.demand;
    }
    // Generators that are not in a live island cannot be overloaded.
    for (NodeIndex i : members) {
        if (IsSource(def_.nodes[i].type) && live[i] == 0 && Pending(i, TimerKind::OverloadTrip)) Cancel(i, TimerKind::OverloadTrip);
    }
    if (status.overloaded && !wasOverloaded) {
        const double ratio = status.supplyKw > 0.0 ? status.demandKw / status.supplyKw : 0.0;
        Emit(FacilityEventType::NetworkOverloaded, members.front(), static_cast<std::int64_t>(ratio * 1000.0), status.name);
    }

    // Batteries: rates and their next empty/full event.
    for (NodeIndex b : members) {
        if (def_.nodes[b].type != NodeType::Battery) continue;
        NodeState& s = nodes_[b];
        s.batteryRateKw = batteryRate[b];
        Cancel(b, TimerKind::BatteryEmpty);
        Cancel(b, TimerKind::BatteryFull);
        if (s.batteryRateKw > kEps) {
            Schedule(b, TimerKind::BatteryEmpty, static_cast<Tick>(std::ceil(s.charge / s.batteryRateKw)));
        } else if (s.batteryRateKw < -kEps) {
            const double room = def_.nodes[b].batteryCapacityKwTicks - s.charge;
            Schedule(b, TimerKind::BatteryFull, static_cast<Tick>(std::ceil(room / -s.batteryRateKw)));
        }
    }

    // Breaker loads: powered demand reachable downstream.
    std::vector<char> seen(total, 0);
    for (NodeIndex br : members) {
        if (def_.nodes[br].type != NodeType::Breaker) continue;
        double load = 0.0;
        if (live[br] != 0) {
            std::fill(seen.begin(), seen.end(), 0);
            std::vector<NodeIndex> stack(children_[br].begin(), children_[br].end());
            while (!stack.empty()) {
                const NodeIndex x = stack.back();
                stack.pop_back();
                if (seen[x] != 0) continue;
                seen[x] = 1;
                if (IsConsumer(def_.nodes[x].type)) {
                    if (newPowered[x] != 0) load += def_.nodes[x].demandKw;
                } else if (live[x] != 0) {
                    for (NodeIndex c : children_[x]) stack.push_back(c);
                }
            }
        }
        nodes_[br].loadKw = load;
        const bool over = load > def_.nodes[br].capacityKw + kEps;
        if (over && !Pending(br, TimerKind::BreakerTrip)) Schedule(br, TimerKind::BreakerTrip, def_.nodes[br].tripDelayTicks);
        if (!over && Pending(br, TimerKind::BreakerTrip)) Cancel(br, TimerKind::BreakerTrip);
    }

    // Commit energisation and power, then run behaviour for changes.
    std::vector<NodeIndex> changed;
    for (NodeIndex i : members) {
        NodeState& s = nodes_[i];
        const bool en = live[i] != 0;
        if (en != s.energized) {
            s.energized = en;
            Changed(i);
        }
        if (!IsConsumer(def_.nodes[i].type)) continue;
        const bool p = newPowered[i] != 0;
        if (p != s.powered) {
            s.powered = p;
            changed.push_back(i);
        }
    }
    for (NodeIndex i : changed) OnPowerChanged(i, !nodes_[i].powered);

    // Backup generators: start when primary supply to their bus is lost.
    std::vector<char> primary(total, 0);
    Energize(network, false, primary);
    for (NodeIndex b : members) {
        const NodeDef& d = def_.nodes[b];
        if (d.type != NodeType::BackupGenerator || !d.autoStart) continue;
        NodeState& s = nodes_[b];
        const bool primaryUp = std::any_of(children_[b].begin(), children_[b].end(), [&](NodeIndex c) { return primary[c] != 0; });
        if (primaryUp) s.everPowered = true;  // armed: it has seen primary power
        if (!primaryUp && s.everPowered && s.gen == GeneratorState::Off && !s.destroyed && !Pending(b, TimerKind::BackupAutoStart)) {
            Schedule(b, TimerKind::BackupAutoStart, d.autoStartDelayTicks);
        }
        if (primaryUp && Pending(b, TimerKind::BackupAutoStart)) Cancel(b, TimerKind::BackupAutoStart);
        if (primaryUp && s.autoStarted && d.autoStopOnRestore &&
            (s.gen == GeneratorState::Running || s.gen == GeneratorState::Starting)) {
            StopGenerator(b);
        }
    }
}

void Facility::OnPowerChanged(NodeIndex i, bool wasPowered) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    const bool powered = s.powered;
    const bool restored = powered && !wasPowered && s.everPowered;
    if (powered) s.everPowered = true;
    Changed(i);
    Emit(FacilityEventType::PowerChanged, i, powered ? 1 : 0);

    switch (d.type) {
        case NodeType::Door:
        case NodeType::SecurityGate:
            if (!powered && (s.door == DoorState::Opening || s.door == DoorState::Closing)) {
                // Motor stalls: the door falls back to where it was.
                Cancel(i, TimerKind::DoorMotionDone);
                s.door = s.door == DoorState::Opening ? DoorState::Closed : DoorState::Open;
                Emit(FacilityEventType::DoorState, i, static_cast<std::int64_t>(s.door));
            }
            if (powered) {
                // Motor available: manual cranking is pointless now.
                channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                               [i](const Channel& c) { return c.node == i && c.kind == ChannelKind::Crank; }),
                                channels_.end());
            }
            break;
        case NodeType::Light:
            UpdateLight(i);
            break;
        case NodeType::EmergencyLight:
            if (powered) {
                s.onEmergency = false;
                Cancel(i, TimerKind::EmergencyEnd);
            } else if (wasPowered && !s.destroyed) {
                s.onEmergency = true;  // backup system kicks in on power loss
                Schedule(i, TimerKind::EmergencyEnd, d.emergencyBatteryTicks);
            }
            UpdateLight(i);
            break;
        case NodeType::Machinery:
            UpdateContinuousSound(i);
            break;
        case NodeType::Alarm:
            if (powered) {
                if (s.alarmActive) {
                    Schedule(i, TimerKind::AlarmPulse, 1);  // latched alarm resumes
                    if (d.alarmDurationTicks > 0 && !Pending(i, TimerKind::AlarmEnd)) {
                        Schedule(i, TimerKind::AlarmEnd, d.alarmDurationTicks);
                    }
                } else if (restored && d.triggerOnPowerRestore) {
                    SetAlarm(i, true);  // security reboot
                }
            } else {
                Cancel(i, TimerKind::AlarmPulse);
                Cancel(i, TimerKind::AlarmEnd);  // the countdown pauses with the siren
            }
            break;
        case NodeType::SecuritySensor:
            if (!powered && !d.rearmOnPowerRestore) s.armCommanded = false;
            Emit(FacilityEventType::SecurityArmed, i, (powered && s.armCommanded) ? 1 : 0);
            break;
        case NodeType::CommArray:
            if (powered && s.switchedOn) {
                if (!d.objectiveSignal.empty()) Emit(FacilityEventType::ObjectiveSignal, i, 1, d.objectiveSignal);
                Schedule(i, TimerKind::NoisePulse, 1);
            } else {
                Cancel(i, TimerKind::NoisePulse);
            }
            break;
        default:
            break;
    }
    if (d.type == NodeType::Alarm) {
        const std::size_t now = ActiveAlarms();
        if (now != lastActiveAlarms_) {
            lastActiveAlarms_ = now;
            Emit(FacilityEventType::AlertLevelChanged, i, static_cast<std::int64_t>(now));
        }
    }
}

// ===========================================================================
// Behaviour
// ===========================================================================

void Facility::SetGenerator(NodeIndex i, GeneratorState next) {
    NodeState& s = nodes_[i];
    if (s.gen == next) return;
    if (s.gen == GeneratorState::Running) {
        IntegrateFuel(i);
        Cancel(i, TimerKind::FuelEmpty);
        Cancel(i, TimerKind::OverloadTrip);
        Cancel(i, TimerKind::NoisePulse);
    }
    s.gen = next;
    if (next == GeneratorState::Running) {
        s.fuelStamp = tick_;
        if (s.fuel >= 0.0) Schedule(i, TimerKind::FuelEmpty, static_cast<Tick>(std::ceil(s.fuel)));
        Schedule(i, TimerKind::NoisePulse, def_.nodes[i].noisePulseTicks);
    }
    UpdateContinuousSound(i);
    Changed(i);
    Emit(FacilityEventType::GeneratorState, i, static_cast<std::int64_t>(next));
    MarkNetworkDirty(i);
}

InteractionResult Facility::StartGenerator(NodeIndex i, PlayerId by) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    if (s.destroyed) return InteractionResult::RejectedDestroyed;
    if (s.gen == GeneratorState::Running || s.gen == GeneratorState::Starting) return InteractionResult::RejectedInvalidState;
    if (s.fuel >= 0.0 && s.fuel < 1.0) return InteractionResult::RejectedInvalidState;  // dry
    ++s.startAttempts;
    Changed(i);
    if (IsDamaged(i)) {
        const double roll = HashToUnit(HashCombine(HashCombine(def_.seed, i), s.startAttempts));
        if (roll < d.startFailChanceDamaged) {
            // Damaged engines can backfire: loud, and nothing starts.
            Sound(SoundKind::GeneratorFailure, i, d.noiseDb + 15.0, 3);
            Emit(FacilityEventType::GeneratorState, i, static_cast<std::int64_t>(s.gen), "start_failed");
            return InteractionResult::Applied;
        }
    }
    (void)by;
    SetGenerator(i, GeneratorState::Starting);
    Sound(SoundKind::GeneratorStart, i, d.noiseDb + 6.0, d.startupTicks);
    Schedule(i, TimerKind::GeneratorStarted, d.startupTicks);
    return InteractionResult::Applied;
}

void Facility::StopGenerator(NodeIndex i) {
    Cancel(i, TimerKind::GeneratorStarted);
    nodes_[i].autoStarted = false;
    SetGenerator(i, GeneratorState::Off);
}

void Facility::TripBreaker(NodeIndex i) {
    NodeState& s = nodes_[i];
    if (s.tripped) return;
    s.tripped = true;
    Changed(i);
    Sound(SoundKind::BreakerTrip, i, 78.0, 1);
    Emit(FacilityEventType::BreakerTripped, i, static_cast<std::int64_t>(s.loadKw * 1000.0));
    Emit(FacilityEventType::BreakerState, i, 0);
    MarkNetworkDirty(i);
}

InteractionResult Facility::OperateDoor(NodeIndex i, bool open, PlayerId by, bool remote) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    if (s.destroyed) return InteractionResult::RejectedDestroyed;
    if (s.door == DoorState::Opening || s.door == DoorState::Closing) return InteractionResult::RejectedBusy;
    if (remote) {
        // Terminal command: unlock+open, or close+lock.
        if (open) s.lockCommanded = false;
        else s.lockCommanded = true;
        Changed(i);
        if (s.door == (open ? DoorState::Open : DoorState::Closed)) return InteractionResult::Applied;
    } else if (s.door == (open ? DoorState::Open : DoorState::Closed)) {
        return InteractionResult::RejectedInvalidState;
    }
    if (!s.powered) return remote ? InteractionResult::Applied : InteractionResult::RejectedNoPower;
    if (open && IsLocked(i)) {
        if (d.keyItem.empty()) return InteractionResult::RejectedLocked;
        if (inventory_ == nullptr || !inventory_->HasItem(by, d.keyItem)) return InteractionResult::RejectedMissingItem;
        s.lockCommanded = false;  // keycard opens the lock; it is not consumed
    }
    s.door = open ? DoorState::Opening : DoorState::Closing;
    Schedule(i, TimerKind::DoorMotionDone, d.motorTicks);
    Sound(SoundKind::DoorMotor, i, 58.0, d.motorTicks);
    Changed(i);
    Emit(FacilityEventType::DoorState, i, static_cast<std::int64_t>(s.door));
    return InteractionResult::Applied;
}

void Facility::FinishDoorMotion(NodeIndex i) {
    NodeState& s = nodes_[i];
    if (s.door == DoorState::Opening) s.door = DoorState::Open;
    else if (s.door == DoorState::Closing) s.door = DoorState::Closed;
    else return;
    Changed(i);
    Emit(FacilityEventType::DoorState, i, static_cast<std::int64_t>(s.door));
    Emit(FacilityEventType::NavBlockChanged, i, s.door == DoorState::Open ? 0 : 1);
}

void Facility::SetAlarm(NodeIndex i, bool active) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    if (s.alarmActive == active || (active && s.destroyed)) return;
    s.alarmActive = active;
    Changed(i);
    Emit(active ? FacilityEventType::AlarmTriggered : FacilityEventType::AlarmSilenced, i, s.powered ? 1 : 0);
    if (active) {
        if (s.powered) {
            Sound(SoundKind::AlarmPulse, i, d.noiseDb > 0.0 ? d.noiseDb : kAlarmDefaultDb, d.alarmIntervalTicks);
            Schedule(i, TimerKind::AlarmPulse, d.alarmIntervalTicks);
            if (d.alarmDurationTicks > 0) Schedule(i, TimerKind::AlarmEnd, d.alarmDurationTicks);
        }
        // An unpowered alarm stays latched and sounds when power returns.
    } else {
        Cancel(i, TimerKind::AlarmPulse);
        Cancel(i, TimerKind::AlarmEnd);
    }
    const std::size_t now = ActiveAlarms();
    if (now != lastActiveAlarms_) {
        lastActiveAlarms_ = now;
        Emit(FacilityEventType::AlertLevelChanged, i, static_cast<std::int64_t>(now));
    }
}

void Facility::UpdateLight(NodeIndex i) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    bool lit = false;
    if (!s.destroyed) {
        lit = d.type == NodeType::Light ? (s.powered && s.switchedOn) : s.onEmergency;
    }
    if (lit == s.lit) return;
    s.lit = lit;
    Changed(i);
    const double intensity = s.onEmergency ? d.emergencyIntensity : d.lightIntensity;
    Emit(FacilityEventType::LightChanged, i, lit ? static_cast<std::int64_t>(intensity * 1000.0) : 0);
}

void Facility::UpdateContinuousSound(NodeIndex i) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    bool active = false;
    if (d.type == NodeType::Generator || d.type == NodeType::BackupGenerator) active = s.gen == GeneratorState::Running;
    if (d.type == NodeType::Machinery) active = s.powered && s.switchedOn && !s.destroyed;
    if (active == s.soundActive) return;
    s.soundActive = active;
    Changed(i);
    if (d.type == NodeType::Machinery) {
        if (active) Schedule(i, TimerKind::NoisePulse, 1);
        else Cancel(i, TimerKind::NoisePulse);
        if (d.blocksNavWhenRunning) Emit(FacilityEventType::NavBlockChanged, i, active ? 1 : 0);
    }
    if (sound_ != nullptr && d.noiseDb > 0.0) sound_->OnContinuousSound(i, d.position, d.noiseDb, active);
}

void Facility::NotifySensors(const Vec3& position) {
    for (NodeIndex s : sensors_) {
        const NodeState& st = nodes_[s];
        const NodeDef& d = def_.nodes[s];
        if (!st.powered || !st.armCommanded || st.destroyed || d.alarm.empty()) continue;
        if (Distance(d.position, position) > d.sensorRadiusCm) continue;
        SetAlarm(Find(d.alarm), true);
    }
}

bool Facility::ServerTriggerAlarm(NodeIndex alarm) {
    if (role_ != AuthorityRole::Server || alarm >= nodes_.size() || def_.nodes[alarm].type != NodeType::Alarm) return false;
    SetAlarm(alarm, true);
    SolveDirtyNetworks();
    return true;
}

bool Facility::ServerNotifyPlayerPresence(PlayerId player, const Vec3& position) {
    const PlayerSession* p = Player(player);
    if (role_ != AuthorityRole::Server || p == nullptr || !p->connected || !position.IsFinite()) return false;
    NotifySensors(position);
    return true;
}

bool Facility::ServerApplyDamage(NodeIndex i, double amount) {
    if (role_ != AuthorityRole::Server || i >= nodes_.size() || !std::isfinite(amount) || amount <= 0.0) return false;
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    if (s.destroyed) return false;
    const bool wasDamaged = IsDamaged(i);
    s.health = std::max(0.0, s.health - amount);
    Changed(i);
    if (s.health <= 0.0) {
        s.destroyed = true;
        if (d.type == NodeType::Generator || d.type == NodeType::BackupGenerator) {
            Cancel(i, TimerKind::GeneratorStarted);
            Cancel(i, TimerKind::BackupAutoStart);
            s.autoStarted = false;
            SetGenerator(i, GeneratorState::Destroyed);
        }
        if (IsDoorLike(d.type)) {
            Cancel(i, TimerKind::DoorMotionDone);
            if (s.door == DoorState::Opening) s.door = DoorState::Closed;
            if (s.door == DoorState::Closing) s.door = DoorState::Open;
            s.lockBroken = true;
        }
        if (d.type == NodeType::Alarm) SetAlarm(i, false);
        if (d.type == NodeType::Light || d.type == NodeType::EmergencyLight) UpdateLight(i);
        if (d.type == NodeType::Machinery) UpdateContinuousSound(i);
        Emit(FacilityEventType::Destroyed, i);
    } else if (IsDamaged(i) && !wasDamaged) {
        Emit(FacilityEventType::Damaged, i);
    }
    MarkNetworkDirty(i);
    if (started_) SolveDirtyNetworks();
    return true;
}

// ===========================================================================
// Channels (cooperative multi-tick interactions)
// ===========================================================================

double Facility::ChannelRequired(NodeIndex i, ChannelKind kind) const {
    const NodeDef& d = def_.nodes[i];
    switch (kind) {
        case ChannelKind::Crank: return d.manualWork;
        case ChannelKind::Repair: return static_cast<double>(d.repairTicks);
        case ChannelKind::Hack: return static_cast<double>(d.hackTicks);
        case ChannelKind::CallExtraction: return static_cast<double>(d.callTicks);
    }
    return 1.0;
}

std::uint32_t Facility::ChannelMinParticipants(NodeIndex i, ChannelKind kind) const {
    const NodeDef& d = def_.nodes[i];
    return (kind == ChannelKind::Crank || kind == ChannelKind::CallExtraction) ? d.manualParticipants : 1u;
}

InteractionResult Facility::JoinChannel(NodeIndex i, ChannelKind kind, PlayerId player) {
    for (Channel& c : channels_) {
        if (c.node != i) continue;
        if (c.kind != kind) return InteractionResult::RejectedBusy;
        if (std::binary_search(c.participants.begin(), c.participants.end(), player)) return InteractionResult::RejectedInvalidState;
        RemoveFromChannels(player);  // a player works on one thing at a time
        for (Channel& again : channels_) {
            if (again.node == i && again.kind == kind) {
                again.participants.insert(std::lower_bound(again.participants.begin(), again.participants.end(), player), player);
                return InteractionResult::ChannelJoined;
            }
        }
        break;  // channel emptied by RemoveFromChannels; start a new one below
    }
    RemoveFromChannels(player);
    Channel c;
    c.node = i;
    c.kind = kind;
    c.starter = player;
    c.participants.push_back(player);
    channels_.push_back(c);
    return InteractionResult::ChannelStarted;
}

void Facility::RemoveFromChannels(PlayerId player) {
    for (Channel& c : channels_) {
        c.participants.erase(std::remove(c.participants.begin(), c.participants.end(), player), c.participants.end());
    }
    // Progress stays on the node, so a returning squad can resume.
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(), [](const Channel& c) { return c.participants.empty(); }),
                    channels_.end());
}

void Facility::TickChannels() {
    std::vector<Channel> done;
    for (Channel& c : channels_) {
        const NodeDef& d = def_.nodes[c.node];
        NodeState& s = nodes_[c.node];
        c.participants.erase(std::remove_if(c.participants.begin(), c.participants.end(),
                                            [&](PlayerId p) {
                                                const PlayerSession* ps = Player(p);
                                                return ps == nullptr || !ps->connected ||
                                                       Distance(ps->position, d.position) > d.interactRangeCm * config_.rangeTolerance;
                                            }),
                             c.participants.end());
        if (c.participants.empty()) continue;
        const bool needsPower = c.kind == ChannelKind::Hack || c.kind == ChannelKind::CallExtraction;
        if (needsPower && !s.powered) continue;  // paused
        if (c.participants.size() < ChannelMinParticipants(c.node, c.kind)) continue;  // waiting for help
        const double required = ChannelRequired(c.node, c.kind);
        const auto before = static_cast<int>(s.progress / required * 255.0);
        s.progress = std::min(required, s.progress + static_cast<double>(c.participants.size()));
        if (static_cast<int>(s.progress / required * 255.0) != before) Changed(c.node);
        if (s.progress >= required) done.push_back(c);
    }
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(), [](const Channel& c) { return c.participants.empty(); }),
                    channels_.end());
    for (const Channel& c : done) CompleteChannel(c);
}

void Facility::CompleteChannel(const Channel& c) {
    NodeState& s = nodes_[c.node];
    const NodeDef& d = def_.nodes[c.node];
    if (c.kind == ChannelKind::Repair && !d.repairItem.empty()) {
        // The part is used up only when the repair actually finishes.
        bool consumed = false;
        std::vector<PlayerId> order{c.starter};
        order.insert(order.end(), c.participants.begin(), c.participants.end());
        for (PlayerId p : order) {
            const PlayerSession* ps = Player(p);
            if (ps != nullptr && ps->connected && inventory_ != nullptr && inventory_->ConsumeItem(p, d.repairItem)) {
                consumed = true;
                break;
            }
        }
        if (!consumed) return;  // stalls at 100% until someone brings the part
    }
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                   [&c](const Channel& x) { return x.node == c.node && x.kind == c.kind; }),
                    channels_.end());
    s.progress = 0.0;
    Changed(c.node);
    switch (c.kind) {
        case ChannelKind::Crank:
            s.door = s.door == DoorState::Open ? DoorState::Closed : DoorState::Open;
            Sound(SoundKind::DoorMotor, c.node, 64.0, 10);
            Emit(FacilityEventType::DoorState, c.node, static_cast<std::int64_t>(s.door));
            Emit(FacilityEventType::NavBlockChanged, c.node, s.door == DoorState::Open ? 0 : 1);
            break;
        case ChannelKind::Repair:
            s.health = 1.0;
            s.destroyed = false;
            if (s.gen == GeneratorState::Destroyed) s.gen = GeneratorState::Off;
            if (IsDoorLike(d.type)) s.lockBroken = false;
            Emit(FacilityEventType::Repaired, c.node);
            UpdateLight(c.node);
            MarkNetworkDirty(c.node);
            break;
        case ChannelKind::Hack:
            s.hacked = true;
            break;
        case ChannelKind::CallExtraction:
            s.completed = true;
            Emit(FacilityEventType::ObjectiveSignal, c.node, 1, d.objectiveSignal.empty() ? "extraction_called" : d.objectiveSignal);
            Sound(SoundKind::ExtractionBeacon, c.node, d.noiseDb > 0.0 ? d.noiseDb : 100.0, 90);
            break;
    }
    Emit(FacilityEventType::InteractionCompleted, c.node, static_cast<std::int64_t>(c.kind));
}

// ===========================================================================
// Interactions
// ===========================================================================

InteractionResult Facility::Submit(const InteractionRequest& r) {
    if (role_ != AuthorityRole::Server) return Record(InteractionResult::RejectedNotAuthority);
    if (!IsValid() || !started_) return Record(InteractionResult::RejectedInvalidState);
    PlayerSession* p = FindPlayer(r.player);
    if (p == nullptr) return Record(InteractionResult::RejectedUnknownPlayer);
    if (!p->connected) return Record(InteractionResult::RejectedDisconnected);

    // Duplicate requests (retransmits, double clicks, replays) are idempotent.
    const std::size_t window = p->recent.size();
    const bool tooOld = p->highestSeq >= window && r.sequence + window <= p->highestSeq;
    const bool seen = std::find(p->recent.begin(), p->recent.begin() + static_cast<std::ptrdiff_t>(p->recentCount), r.sequence) !=
                      p->recent.begin() + static_cast<std::ptrdiff_t>(p->recentCount);
    if (tooOld || seen) return Record(InteractionResult::RejectedDuplicate);

    if (tick_ > p->tokenStamp) {
        p->tokens = std::min(config_.requestBurst, p->tokens + config_.requestRefillPerTick * static_cast<double>(tick_ - p->tokenStamp));
        p->tokenStamp = tick_;
    }
    if (p->tokens < 1.0) return Record(InteractionResult::RejectedRateLimited);
    p->tokens -= 1.0;
    p->recent[p->recentHead] = r.sequence;
    p->recentHead = (p->recentHead + 1) % window;
    p->recentCount = std::min(p->recentCount + 1, window);
    p->highestSeq = std::max(p->highestSeq, r.sequence);

    if (r.node >= nodes_.size()) return Record(InteractionResult::RejectedUnknownNode);
    const NodeDef& d = def_.nodes[r.node];
    if (Distance(p->position, d.position) > d.interactRangeCm * config_.rangeTolerance) return Record(InteractionResult::RejectedOutOfRange);
    if ((p->permissions & d.requiredPermissions) != d.requiredPermissions) return Record(InteractionResult::RejectedPermission);
    if (r.expectedVersion != 0 && r.expectedVersion != nodes_[r.node].version) return Record(InteractionResult::RejectedStaleVersion);

    InteractionResult result;
    if (r.action == InteractionAction::Command) {
        result = InteractionResult::RejectedUnsupported;
        if (d.type == NodeType::Terminal) {
            const NodeState& t = nodes_[r.node];
            const bool controls = r.commandTarget < nodes_.size() &&
                                  std::find(d.controls.begin(), d.controls.end(), def_.nodes[r.commandTarget].id) != d.controls.end();
            if (!t.powered) result = InteractionResult::RejectedNoPower;
            else if (d.hackTicks > 0 && !t.hacked) result = InteractionResult::RejectedLocked;
            else if (!d.keyItem.empty() && (inventory_ == nullptr || !inventory_->HasItem(r.player, d.keyItem))) result = InteractionResult::RejectedMissingItem;
            else if (!controls) result = InteractionResult::RejectedUnsupported;
            else result = ApplyAction(r.commandTarget, r.commandAction, r.player, true);
        }
    } else {
        result = ApplyAction(r.node, r.action, r.player, false);
    }

    if (Succeeded(result) && d.type != NodeType::Alarm && d.type != NodeType::SecuritySensor) NotifySensors(d.position);
    SolveDirtyNetworks();
    return Record(result);
}

InteractionResult Facility::ApplyAction(NodeIndex i, InteractionAction a, PlayerId by, bool remote) {
    NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    using A = InteractionAction;
    using R = InteractionResult;

    if (a == A::CancelChannel) {
        const auto before = channels_.size();
        for (Channel& c : channels_) {
            if (c.node == i) c.participants.erase(std::remove(c.participants.begin(), c.participants.end(), by), c.participants.end());
        }
        channels_.erase(std::remove_if(channels_.begin(), channels_.end(), [](const Channel& c) { return c.participants.empty(); }),
                        channels_.end());
        (void)before;
        return R::Applied;
    }
    if (a == A::Repair) {
        if (remote) return R::RejectedUnsupported;
        if (!d.repairable) return R::RejectedUnsupported;
        if (s.health >= 1.0 && !s.destroyed) return R::RejectedInvalidState;
        if (!d.repairItem.empty() && (inventory_ == nullptr || !inventory_->HasItem(by, d.repairItem))) return R::RejectedMissingItem;
        return JoinChannel(i, ChannelKind::Repair, by);
    }

    switch (d.type) {
        case NodeType::Generator:
        case NodeType::BackupGenerator:
            if (a == A::Activate) {
                s.autoStarted = false;
                return StartGenerator(i, by);
            }
            if (a == A::Deactivate) {
                if (s.gen != GeneratorState::Running && s.gen != GeneratorState::Starting) return R::RejectedInvalidState;
                StopGenerator(i);
                return R::Applied;
            }
            if (a == A::Refuel && !remote) {
                if (d.fuelItem.empty() || s.fuel < 0.0) return R::RejectedUnsupported;
                if (inventory_ == nullptr || !inventory_->ConsumeItem(by, d.fuelItem)) return R::RejectedMissingItem;
                IntegrateFuel(i);
                s.fuel += d.refuelAmountTicks;
                if (s.gen == GeneratorState::Running) Schedule(i, TimerKind::FuelEmpty, static_cast<Tick>(std::ceil(s.fuel)));
                Changed(i);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::Battery:
            if (a == A::Activate || a == A::Deactivate) {
                if (s.switchedOn == (a == A::Activate)) return R::RejectedInvalidState;
                s.switchedOn = a == A::Activate;
                Changed(i);
                MarkNetworkDirty(i);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::Breaker:
            if (a == A::Activate || a == A::Deactivate) {
                const bool close = a == A::Activate;
                if (close && !s.fuseInstalled) return R::RejectedMissingItem;
                if (close && s.tripped) return R::RejectedInvalidState;
                if (s.closed == close) return R::RejectedInvalidState;
                s.closed = close;
                Changed(i);
                Emit(FacilityEventType::BreakerState, i, Conducts(i) ? 1 : 0);
                MarkNetworkDirty(i);
                return R::Applied;
            }
            if (a == A::ResetBreaker) {
                if (!s.tripped) return R::RejectedInvalidState;
                s.tripped = false;
                s.closed = true;  // may trip again if the load is still there
                Changed(i);
                Emit(FacilityEventType::BreakerState, i, Conducts(i) ? 1 : 0);
                MarkNetworkDirty(i);
                return R::Applied;
            }
            if (a == A::InstallFuse && !remote) {
                if (s.fuseInstalled) return R::RejectedInvalidState;
                if (d.fuseItem.empty() || inventory_ == nullptr || !inventory_->ConsumeItem(by, d.fuseItem)) return R::RejectedMissingItem;
                s.fuseInstalled = true;
                Changed(i);
                MarkNetworkDirty(i);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::Door:
        case NodeType::SecurityGate:
            if (a == A::Activate || a == A::Deactivate) return OperateDoor(i, a == A::Activate, by, remote);
            if (remote) return R::RejectedUnsupported;
            if (a == A::Crank) {
                if (s.destroyed) return R::RejectedDestroyed;
                if (s.powered) return R::RejectedInvalidState;  // use the motor
                if (IsLocked(i)) return R::RejectedLocked;
                return JoinChannel(i, ChannelKind::Crank, by);
            }
            if (a == A::Force) {
                if (!IsLocked(i)) return R::RejectedInvalidState;
                if (s.powered) return R::RejectedLocked;  // a live maglock holds; cut the power first
                if (d.forceItem.empty()) return R::RejectedUnsupported;
                if (inventory_ == nullptr || !inventory_->HasItem(by, d.forceItem)) return R::RejectedMissingItem;
                s.lockBroken = true;
                Changed(i);
                Sound(SoundKind::DoorForced, i, 82.0, 6);
                if (!d.alarm.empty()) SetAlarm(Find(d.alarm), true);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::Light:
        case NodeType::Machinery:
        case NodeType::CommArray:
            if (a == A::Activate || a == A::Deactivate) {
                const bool on = a == A::Activate;
                if (s.switchedOn == on) return R::RejectedInvalidState;
                s.switchedOn = on;  // intent: takes effect whenever power is present
                Changed(i);
                MarkNetworkDirty(i);
                if (d.type == NodeType::Light) UpdateLight(i);
                if (d.type == NodeType::Machinery) UpdateContinuousSound(i);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::Terminal:
            if (remote) return R::RejectedUnsupported;
            if (a == A::Activate) {
                if (!s.powered) return R::RejectedNoPower;
                if (d.hackTicks > 0 && !s.hacked) return R::RejectedLocked;
                return R::Applied;
            }
            if (a == A::Hack) {
                if (!s.powered) return R::RejectedNoPower;
                if (d.hackTicks == 0 || s.hacked) return R::RejectedInvalidState;
                return JoinChannel(i, ChannelKind::Hack, by);
            }
            return R::RejectedUnsupported;

        case NodeType::Alarm:
            if (a == A::TriggerAlarm || a == A::Activate) {
                if (s.destroyed) return R::RejectedDestroyed;
                if (s.alarmActive) return R::RejectedInvalidState;
                SetAlarm(i, true);
                return R::Applied;
            }
            if (a == A::SilenceAlarm || a == A::Deactivate) {
                if (!s.alarmActive) return R::RejectedInvalidState;
                SetAlarm(i, false);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::SecuritySensor:
            if (a == A::Activate || a == A::Deactivate) {
                const bool arm = a == A::Activate;
                if (s.armCommanded == arm) return R::RejectedInvalidState;
                s.armCommanded = arm;
                Changed(i);
                Emit(FacilityEventType::SecurityArmed, i, (s.powered && arm) ? 1 : 0);
                return R::Applied;
            }
            return R::RejectedUnsupported;

        case NodeType::ExtractionPoint:
            if (a == A::CallExtraction && !remote) {
                if (s.completed) return R::RejectedInvalidState;
                if (!s.powered) return R::RejectedNoPower;
                return JoinChannel(i, ChannelKind::CallExtraction, by);
            }
            return R::RejectedUnsupported;

        case NodeType::Junction:
        case NodeType::EmergencyLight:
            return R::RejectedUnsupported;
    }
    return R::RejectedUnsupported;
}

// ===========================================================================
// Simulation step
// ===========================================================================

void Facility::Step() {
    if (role_ != AuthorityRole::Server || !IsValid()) return;
    ++tick_;
    eventsThisTick_ = 0;
    soundsThisTick_ = 0;
    if (!started_) {
        started_ = true;
        // Mission start: infrastructure that was already running keeps running.
        for (NodeIndex i = 0; i < nodes_.size(); ++i) {
            const NodeDef& d = def_.nodes[i];
            const bool gen = d.type == NodeType::Generator || d.type == NodeType::BackupGenerator;
            if (gen && d.startsRunning && !nodes_[i].destroyed && (nodes_[i].fuel < 0.0 || nodes_[i].fuel >= 1.0)) {
                SetGenerator(i, GeneratorState::Running);
            }
        }
        for (auto& dirty : networkDirty_) dirty = 1;
    }
    FireTimers();
    TickChannels();
    SolveDirtyNetworks();
}

// ===========================================================================
// Replication
// ===========================================================================

NodeReplicationState Facility::MakeReplicationState(NodeIndex i) const {
    const NodeState& s = nodes_[i];
    const NodeDef& d = def_.nodes[i];
    NodeReplicationState r;
    r.node = i;
    r.version = s.version;
    r.primaryState = IsDoorLike(d.type) ? static_cast<std::uint8_t>(s.door) : static_cast<std::uint8_t>(s.gen);
    std::uint16_t f = 0;
    if (s.powered) f |= RepFlag::Powered;
    if (s.energized) f |= RepFlag::Energized;
    if (IsLocked(i)) f |= RepFlag::Locked;
    if (s.lit) f |= RepFlag::Lit;
    if (s.alarmActive) f |= RepFlag::AlarmActive;
    if (s.tripped) f |= RepFlag::Tripped;
    if (d.type == NodeType::Breaker ? s.closed : s.switchedOn) f |= RepFlag::OnOrClosed;
    if (IsDamaged(i)) f |= RepFlag::Damaged;
    if (s.destroyed) f |= RepFlag::Destroyed;
    if (s.powered && s.armCommanded && d.type == NodeType::SecuritySensor) f |= RepFlag::Armed;
    if (s.hacked) f |= RepFlag::Hacked;
    if (s.completed) f |= RepFlag::Completed;
    if (s.onEmergency) f |= RepFlag::Emergency;
    r.flags = f;
    ChannelKind kind = ChannelKind::Repair;
    if (IsDoorLike(d.type)) kind = ChannelKind::Crank;
    if (d.type == NodeType::Terminal) kind = ChannelKind::Hack;
    if (d.type == NodeType::ExtractionPoint) kind = ChannelKind::CallExtraction;
    for (const Channel& c : channels_) {
        if (c.node == i) kind = c.kind;
    }
    const double req = std::max(1.0, ChannelRequired(i, kind));
    r.progress = static_cast<std::uint8_t>(std::clamp(s.progress / req, 0.0, 1.0) * 255.0);
    return r;
}

void Facility::CollectReplication(std::vector<NodeReplicationState>& out) {
    if (role_ != AuthorityRole::Server) return;
    for (NodeIndex i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].replicationDirty) continue;
        nodes_[i].replicationDirty = false;
        out.push_back(MakeReplicationState(i));
    }
}

std::size_t Facility::ApplyReplication(const std::vector<NodeReplicationState>& states) {
    if (role_ != AuthorityRole::Client) return 0;  // servers never accept mirrored state
    std::size_t applied = 0;
    for (const NodeReplicationState& r : states) {
        if (r.node >= nodes_.size() || r.version <= nodes_[r.node].version) continue;
        NodeState& s = nodes_[r.node];
        const NodeDef& d = def_.nodes[r.node];
        if (IsDoorLike(d.type)) s.door = static_cast<DoorState>(std::min<std::uint8_t>(r.primaryState, 3));
        else s.gen = static_cast<GeneratorState>(std::min<std::uint8_t>(r.primaryState, 4));
        s.powered = (r.flags & RepFlag::Powered) != 0;
        s.energized = (r.flags & RepFlag::Energized) != 0;
        s.lockCommanded = (r.flags & RepFlag::Locked) != 0;
        s.lockBroken = false;
        s.lit = (r.flags & RepFlag::Lit) != 0;
        s.alarmActive = (r.flags & RepFlag::AlarmActive) != 0;
        s.tripped = (r.flags & RepFlag::Tripped) != 0;
        if (d.type == NodeType::Breaker) s.closed = (r.flags & RepFlag::OnOrClosed) != 0;
        else s.switchedOn = (r.flags & RepFlag::OnOrClosed) != 0;
        s.destroyed = (r.flags & RepFlag::Destroyed) != 0;
        s.health = s.destroyed ? 0.0 : ((r.flags & RepFlag::Damaged) != 0 ? d.damagedThreshold * 0.5 : 1.0);
        s.armCommanded = (r.flags & RepFlag::Armed) != 0;
        s.hacked = (r.flags & RepFlag::Hacked) != 0;
        s.completed = (r.flags & RepFlag::Completed) != 0;
        s.onEmergency = (r.flags & RepFlag::Emergency) != 0;
        s.version = r.version;
        ++applied;
    }
    return applied;
}

// ===========================================================================
// Persistence
// ===========================================================================

WorldStateRecord Facility::ExportWorldState(bool extracted) const {
    WorldStateRecord rec;
    rec.facilityId = def_.facilityId;
    for (NodeIndex i = 0; i < nodes_.size(); ++i) {
        const NodeDef& d = def_.nodes[i];
        const bool keep = d.persistence == PersistencePolicy::World ||
                          (d.persistence == PersistencePolicy::WorldOnExtraction && extracted);
        if (!keep) continue;
        const NodeState& s = nodes_[i];
        WorldNodeRecord n;
        n.id = d.id;
        n.health = s.destroyed ? 0.0 : s.health;
        n.fuseInstalled = s.fuseInstalled;
        n.fuel = s.fuel;
        if (s.gen == GeneratorState::Running && s.fuel >= 0.0 && tick_ > s.fuelStamp) {
            n.fuel = std::max(0.0, s.fuel - static_cast<double>(tick_ - s.fuelStamp));
        }
        n.lockBroken = s.lockBroken;
        rec.nodes.push_back(n);
    }
    return rec;
}

ImportReport Facility::ImportWorldState(const WorldStateRecord& rec) {
    ImportReport r;
    if (role_ != AuthorityRole::Server || !IsValid()) {
        r.error = "world state can only be imported by a valid server facility";
        return r;
    }
    if (started_) {
        r.error = "world state can only be imported at mission start";
        return r;
    }
    if (rec.formatVersion != 1) {
        r.error = "unsupported world state format version";
        return r;
    }
    if (rec.facilityId != def_.facilityId) {
        r.error = "world state belongs to facility '" + rec.facilityId + "'";
        return r;
    }
    for (const WorldNodeRecord& n : rec.nodes) {
        const NodeIndex i = Find(n.id);
        if (i == kInvalidNode || def_.nodes[i].persistence == PersistencePolicy::Mission || !std::isfinite(n.health) ||
            !std::isfinite(n.fuel)) {
            r.ignored.push_back(n.id);
            continue;
        }
        const NodeDef& d = def_.nodes[i];
        NodeState& s = nodes_[i];
        s.health = std::clamp(n.health, 0.0, 1.0);
        s.destroyed = s.health <= 0.0;
        if (d.type == NodeType::Generator || d.type == NodeType::BackupGenerator) {
            s.gen = s.destroyed ? GeneratorState::Destroyed : GeneratorState::Off;
            if (d.fuelTicks >= 0.0) s.fuel = std::max(0.0, n.fuel);
        }
        if (d.type == NodeType::Breaker) s.fuseInstalled = n.fuseInstalled;
        if (IsDoorLike(d.type)) s.lockBroken = n.lockBroken;
        Changed(i);
        MarkNetworkDirty(i);
        ++r.applied;
    }
    r.ok = true;
    return r;
}

std::string WorldStateRecord::Serialize() const {
    std::ostringstream os;
    os.precision(17);
    os << "format=" << formatVersion << "\nfacility=" << facilityId << "\n";
    for (const auto& n : nodes) {
        os << "node=" << n.id << '|' << n.health << '|' << (n.fuseInstalled ? 1 : 0) << '|' << n.fuel << '|'
           << (n.lockBroken ? 1 : 0) << "\n";
    }
    return os.str();
}

bool WorldStateRecord::Parse(std::string_view text, WorldStateRecord& out, std::string* error) {
    WorldStateRecord rec;
    auto fail = [error](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    bool sawFormat = false;
    bool sawFacility = false;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("malformed line: " + line);
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "format") {
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0') return fail("bad format version");
            rec.formatVersion = static_cast<std::uint32_t>(v);
            sawFormat = true;
        } else if (key == "facility") {
            rec.facilityId = value;
            sawFacility = true;
        } else if (key == "node") {
            std::vector<std::string> f;
            std::size_t start = 0;
            while (true) {
                const std::size_t bar = value.find('|', start);
                f.push_back(value.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            if (f.size() != 5 || f[0].empty()) return fail("bad node record: " + value);
            WorldNodeRecord n;
            n.id = f[0];
            char* e1 = nullptr;
            char* e2 = nullptr;
            n.health = std::strtod(f[1].c_str(), &e1);
            n.fuel = std::strtod(f[3].c_str(), &e2);
            if (*e1 != '\0' || *e2 != '\0' || (f[2] != "0" && f[2] != "1") || (f[4] != "0" && f[4] != "1")) {
                return fail("bad node values: " + value);
            }
            n.fuseInstalled = f[2] == "1";
            n.lockBroken = f[4] == "1";
            rec.nodes.push_back(n);
        } else {
            return fail("unknown key: " + key);
        }
    }
    if (!sawFormat || !sawFacility) return fail("missing format or facility");
    out = rec;
    return true;
}

MissionSnapshot Facility::Capture() const {
    MissionSnapshot m;
    m.tick = tick_;
    m.started = started_;
    m.nodes = nodes_;
    m.timers = timers_;
    m.timerSeq = timerSeq_;
    m.channels = channels_;
    m.players = players_;
    return m;
}

bool Facility::Restore(const MissionSnapshot& m) {
    if (role_ != AuthorityRole::Server || m.nodes.size() != nodes_.size()) return false;
    tick_ = m.tick;
    started_ = m.started;
    nodes_ = m.nodes;
    timers_ = m.timers;
    timerSeq_ = m.timerSeq;
    channels_ = m.channels;
    players_ = m.players;
    lastActiveAlarms_ = ActiveAlarms();
    for (NodeIndex i = 0; i < nodes_.size(); ++i) {
        nodes_[i].replicationDirty = true;
        // Re-sync continuous sounds with the acoustic system.
        if (sound_ != nullptr && def_.nodes[i].noiseDb > 0.0 &&
            (def_.nodes[i].type == NodeType::Machinery || def_.nodes[i].type == NodeType::Generator ||
             def_.nodes[i].type == NodeType::BackupGenerator)) {
            sound_->OnContinuousSound(i, def_.nodes[i].position, def_.nodes[i].noiseDb, nodes_[i].soundActive);
        }
    }
    for (auto& d : networkDirty_) d = 0;
    return true;
}

std::uint64_t Facility::StateHash() const {
    StateHasher h;
    h.Add(tick_);
    for (const NodeState& s : nodes_) {
        h.AddDouble(s.health);
        h.Add((s.destroyed ? 1u : 0u) | (s.energized ? 2u : 0u) | (s.powered ? 4u : 0u) | (s.switchedOn ? 8u : 0u) |
              (s.closed ? 16u : 0u) | (s.tripped ? 32u : 0u) | (s.fuseInstalled ? 64u : 0u) | (s.lockCommanded ? 128u : 0u) |
              (s.lockBroken ? 256u : 0u) | (s.lit ? 512u : 0u) | (s.onEmergency ? 1024u : 0u) | (s.alarmActive ? 2048u : 0u) |
              (s.hacked ? 4096u : 0u) | (s.completed ? 8192u : 0u) | (s.armCommanded ? 16384u : 0u) |
              (s.soundActive ? 32768u : 0u) | (s.autoStarted ? 65536u : 0u));
        h.Add(static_cast<std::uint64_t>(s.gen));
        h.Add(static_cast<std::uint64_t>(s.door));
        h.AddDouble(s.fuel);
        h.AddDouble(s.charge);
        h.AddDouble(s.batteryRateKw);
        h.AddDouble(s.progress);
        h.Add(s.version);
        for (std::size_t k = 0; k < kTimerKindCount; ++k) h.Add(s.timerPending[k] ? 1u : 0u);
    }
    h.Add(channels_.size());
    for (const Channel& c : channels_) {
        h.Add(c.node);
        h.Add(static_cast<std::uint64_t>(c.kind));
        for (PlayerId p : c.participants) h.Add(p);
    }
    return h.Value();
}

}  // namespace llenv
