// LAST LIGHT - Environmental & World Intelligence (WI)
#include "llworld/Squad.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace llworld {

const char* ToString(ThreatClass t) {
    switch (t) {
        case ThreatClass::Disturbance: return "Disturbance";
        case ThreatClass::Movement: return "Movement";
        case ThreatClass::Infrastructure: return "Infrastructure";
        case ThreatClass::Hostile: return "Hostile";
    }
    return "Unknown";
}

const char* ToString(IntentKind k) {
    switch (k) {
        case IntentKind::Patrol: return "Patrol";
        case IntentKind::Investigate: return "Investigate";
        case IntentKind::Search: return "Search";
        case IntentKind::Observe: return "Observe";
        case IntentKind::Flank: return "Flank";
        case IntentKind::Defend: return "Defend";
        case IntentKind::LocalAutonomous: return "LocalAutonomous";
    }
    return "Unknown";
}

const char* ToString(ReportResult r) {
    switch (r) {
        case ReportResult::Accepted: return "Accepted";
        case ReportResult::Duplicate: return "Duplicate";
        case ReportResult::UnknownRobot: return "UnknownRobot";
        case ReportResult::RobotDisabled: return "RobotDisabled";
        case ReportResult::Invalid: return "Invalid";
    }
    return "Unknown";
}

std::vector<RoleDef> SquadConfig::DefaultRoles() {
    RoleDef sentinel;
    sentinel.name = "SENTINEL";
    sentinel.canInvestigate = true;
    sentinel.canObserve = false;
    sentinel.canFlank = true;
    sentinel.canDefend = true;
    sentinel.investigateWeight = 1.2;
    sentinel.commsRangeCm = 5000.0;
    sentinel.observationStandoffCm = 1200.0;
    sentinel.reportQuality = 1.0;

    RoleDef wraith;
    wraith.name = "WRAITH";
    wraith.canInvestigate = true;
    wraith.canObserve = true;
    wraith.canFlank = false;
    wraith.canDefend = false;
    wraith.investigateWeight = 0.6;  // prefers to watch and report
    wraith.commsRangeCm = 9000.0;
    wraith.observationStandoffCm = 2000.0;
    wraith.reportQuality = 1.1;
    return {sentinel, wraith};
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

std::vector<std::string> ValidateSquads(const SquadConfig& c) {
    std::vector<std::string> e;
    auto require = [&e](bool ok, const std::string& m) {
        if (!ok) e.push_back(m);
    };
    require(c.ticksPerSecond > 0.0 && c.coordinationIntervalTicks >= 1, "squads: tick settings invalid");
    require(c.confidenceHalfLifeTicks > 0.0 && c.forgetConfidence >= 0.0 && c.forgetConfidence < c.investigateMinConfidence &&
                c.investigateMinConfidence <= 1.0,
            "squads: need 0 <= forget_confidence < investigate_min_confidence <= 1");
    require(c.sharedConfidenceMul > 0.0 && c.sharedConfidenceMul <= 1.0, "squads: shared_confidence_mul must be in (0,1]");
    require(c.negativeReportFactor >= 0.0 && c.negativeReportFactor < 1.0, "squads: negative_report_factor must be in [0,1)");
    require(c.minPatrolFraction >= 0.0 && c.minPatrolFraction < 1.0, "squads: min_patrol_fraction must be in [0,1)");
    require(c.damagedCommsRangeMul > 0.0 && c.damagedCommsRangeMul <= 1.0, "squads: damaged_comms_range_mul must be in (0,1]");
    require(c.damagedDropChance >= 0.0 && c.damagedDropChance <= 1.0, "squads: damaged_drop_chance must be in [0,1]");
    require(c.mergeRadiusCm > 0.0 && c.arrivalRadiusCm > 0.0, "squads: radii must be > 0");
    require(c.maxContacts >= 1 && c.maxPendingReports >= 1 && c.dedupHistory >= 1, "squads: capacities must be >= 1");
    for (std::size_t i = 0; i < c.roles.size(); ++i) {
        const RoleDef& r = c.roles[i];
        require(!r.name.empty(), "role with empty name");
        for (std::size_t j = 0; j < i; ++j) require(c.roles[j].name != r.name, "role." + r.name + ": duplicate");
        require(r.commsRangeCm > 0.0 && r.observationStandoffCm > 0.0 && r.investigateWeight > 0.0 && r.reportQuality > 0.0,
                "role." + r.name + ": ranges/weights must be > 0");
    }
    std::vector<RobotId> robots;
    for (const SquadDef& s : c.squads) {
        const std::string n = "squad." + s.id + ": ";
        require(!s.members.empty() && s.members.size() <= 32, n + "needs 1..32 members");
        require(s.relayNode.empty() || s.relayRangeCm > 0.0, n + "relay_node needs relay_range_cm");
        for (const SquadMemberDef& m : s.members) {
            require(m.robot != 0, n + "robot id 0 is reserved");
            require(std::find(robots.begin(), robots.end(), m.robot) == robots.end(), n + "robot " + std::to_string(m.robot) + " is in two squads");
            robots.push_back(m.robot);
            const bool known = std::any_of(c.roles.begin(), c.roles.end(), [&m](const RoleDef& r) { return r.name == m.role; });
            require(known, n + "unknown role '" + m.role + "'");
        }
    }
    return e;
}

void LoadSquads(const IniDocument& doc, SquadConfig& c, std::vector<std::string>& errors) {
    for (const IniSection& s : doc.sections) {
        if (s.name == "squads") {
            IniBinder b;
            b.Ticks("coordination_interval_ticks", c.coordinationIntervalTicks);
            b.Ticks("report_latency_ticks", c.reportLatencyTicks);
            b.Num("merge_radius_cm", c.mergeRadiusCm);
            b.Num("confidence_half_life_ticks", c.confidenceHalfLifeTicks);
            b.Num("forget_confidence", c.forgetConfidence);
            b.Num("investigate_min_confidence", c.investigateMinConfidence);
            b.Num("shared_confidence_mul", c.sharedConfidenceMul);
            b.Num("negative_report_factor", c.negativeReportFactor);
            b.Num("min_patrol_fraction", c.minPatrolFraction);
            b.Num("hostile_flank_confidence", c.hostileFlankConfidence);
            b.Ticks("stale_order_ticks", c.staleOrderTicks);
            b.Num("damaged_comms_range_mul", c.damagedCommsRangeMul);
            b.Num("damaged_drop_chance", c.damagedDropChance);
            b.Apply(s, errors);
        } else if (s.name.rfind("role.", 0) == 0) {
            const std::string name = s.name.substr(5);
            RoleDef* r = nullptr;
            for (auto& existing : c.roles) {
                if (existing.name == name) r = &existing;
            }
            if (r == nullptr) {
                RoleDef fresh;
                fresh.name = name;
                c.roles.push_back(fresh);
                r = &c.roles.back();
            }
            IniBinder b;
            b.Flag("can_investigate", r->canInvestigate);
            b.Flag("can_observe", r->canObserve);
            b.Flag("can_flank", r->canFlank);
            b.Flag("can_defend", r->canDefend);
            b.Num("investigate_weight", r->investigateWeight);
            b.Num("comms_range_cm", r->commsRangeCm);
            b.Num("observation_standoff_cm", r->observationStandoffCm);
            b.Num("report_quality", r->reportQuality);
            b.Apply(s, errors);
        } else if (s.name.rfind("squad.", 0) == 0) {
            SquadDef q;
            q.id = s.name.substr(6);
            IniBinder b;
            b.Custom("members", [&q](std::string_view v) {
                q.members.clear();
                std::string_view rest = v;
                while (!rest.empty()) {
                    const std::size_t comma = rest.find(',');
                    const std::string_view item = TrimView(comma == std::string_view::npos ? rest : rest.substr(0, comma));
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                    const std::size_t colon = item.find(':');
                    double id = 0.0;
                    if (colon == std::string_view::npos || !ParseNumber(item.substr(0, colon), id) || id < 1 || id != std::floor(id)) return false;
                    SquadMemberDef m;
                    m.robot = static_cast<RobotId>(id);
                    m.role = std::string(TrimView(item.substr(colon + 1)));
                    q.members.push_back(m);
                }
                return !q.members.empty();
            });
            b.Str("relay_node", q.relayNode);
            b.Point("relay_position", q.relayPosition);
            b.Num("relay_range_cm", q.relayRangeCm);
            b.Point("defend_point", q.defendPoint);
            // patrol.<robot> = x,y,z; x,y,z
            std::vector<std::pair<std::string, std::vector<Vec3>>> routes;
            for (const IniEntry& e : s.entries) {
                if (e.key.rfind("patrol.", 0) == 0) routes.emplace_back(e.key, std::vector<Vec3>{});
            }
            for (auto& r : routes) b.Points(r.first.c_str(), r.second);
            b.Apply(s, errors);
            for (auto& r : routes) {
                double id = 0.0;
                bool matched = false;
                if (ParseNumber(std::string_view(r.first).substr(7), id)) {
                    for (auto& m : q.members) {
                        if (static_cast<double>(m.robot) == id) {
                            m.patrol = r.second;
                            matched = true;
                        }
                    }
                }
                if (!matched) errors.push_back("[" + s.name + "] " + r.first + " does not match a member");
            }
            c.squads.push_back(q);
        }
    }
}

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

SquadCoordinator::SquadCoordinator(const SquadConfig& config, WorldEventBus* bus, ISquadWorld* world)
    : cfg_(config), bus_(bus), world_(world) {
    errors_ = ValidateSquads(cfg_);
    if (!errors_.empty()) return;
    squadMembers_.resize(cfg_.squads.size());
    for (std::uint32_t s = 0; s < cfg_.squads.size(); ++s) {
        const auto& sq = cfg_.squads[s];
        for (std::uint32_t k = 0; k < sq.members.size(); ++k) {
            Member m;
            m.id = sq.members[k].robot;
            m.squad = s;
            m.slot = k;
            for (std::uint32_t r = 0; r < cfg_.roles.size(); ++r) {
                if (cfg_.roles[r].name == sq.members[k].role) m.role = r;
            }
            if (!sq.members[k].patrol.empty()) m.position = sq.members[k].patrol.front();
            members_.push_back(m);
        }
    }
    std::sort(members_.begin(), members_.end(), [](const Member& a, const Member& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < members_.size(); ++i) {
        members_[i].intent = PatrolIntent(members_[i]);
        squadMembers_[members_[i].squad].push_back(i);
    }
    seen_.assign(cfg_.dedupHistory, {0, 0});
}

SquadCoordinator::Member* SquadCoordinator::FindMember(RobotId id) {
    auto it = std::lower_bound(members_.begin(), members_.end(), id, [](const Member& m, RobotId v) { return m.id < v; });
    return (it != members_.end() && it->id == id) ? &*it : nullptr;
}
const SquadCoordinator::Member* SquadCoordinator::FindMember(RobotId id) const {
    auto it = std::lower_bound(members_.begin(), members_.end(), id, [](const Member& m, RobotId v) { return m.id < v; });
    return (it != members_.end() && it->id == id) ? &*it : nullptr;
}

bool SquadCoordinator::UpdateRobot(RobotId robot, const Vec3& position, bool alive, CommsHealth comms) {
    Member* m = FindMember(robot);
    if (m == nullptr || !position.IsFinite()) return false;
    m->position = position;
    m->alive = alive;
    m->comms = comms;
    return true;
}

ReportResult SquadCoordinator::Report(const ContactReport& r) {
    if (!IsValid()) return ReportResult::Invalid;
    Member* m = FindMember(r.reporter);
    if (m == nullptr) return ReportResult::UnknownRobot;
    if (!m->alive) return ReportResult::RobotDisabled;
    if (!r.estimate.IsFinite() || !(r.confidence >= 0.0 && r.confidence <= 1.0) || !(r.uncertaintyCm >= 0.0) ||
        !std::isfinite(r.uncertaintyCm) || static_cast<std::size_t>(r.threat) >= kThreatClassCount) {
        return ReportResult::Invalid;
    }
    for (const auto& s : seen_) {
        if (s.first == r.reporter && s.second == r.reportId) return ReportResult::Duplicate;
    }
    seen_[seenHead_] = {r.reporter, r.reportId};
    seenHead_ = (seenHead_ + 1) % seen_.size();

    const double own = Clamp01(r.confidence * cfg_.roles[m->role].reportQuality);
    Fuse(*m, r, own, m->slot);
    if (pending_.size() >= cfg_.maxPendingReports) {
        pending_.erase(pending_.begin());  // oldest gossip is dropped first
        ++dropped_;
    }
    ContactReport shared = r;
    shared.confidence = own;
    pending_.push_back(Pending{shared, static_cast<std::size_t>(m - members_.data()), r.tick + cfg_.reportLatencyTicks});
    return ReportResult::Accepted;
}

void SquadCoordinator::Fuse(Member& m, const ContactReport& r, double confidence, std::uint32_t sourceSlot) {
    Contact* best = nullptr;
    double bestD = 1e300;
    for (Contact& c : m.board) {
        const double d = Distance(c.estimate, r.estimate);
        const double gate = cfg_.mergeRadiusCm + 0.5 * (c.uncertaintyCm + r.uncertaintyCm);
        if (d <= gate && d < bestD) {
            best = &c;
            bestD = d;
        }
    }
    if (r.kind == ReportKind::NegativeSearch) {
        // Conflicting evidence: someone looked and found nothing.
        if (best != nullptr) {
            best->confidence *= cfg_.negativeReportFactor;
            best->cleared = true;
        }
        return;
    }
    if (best != nullptr) {
        const double u1 = std::max(50.0, best->uncertaintyCm);
        const double u2 = std::max(50.0, r.uncertaintyCm);
        const double w1 = 1.0 / (u1 * u1);
        const double w2 = confidence / (u2 * u2);
        best->estimate = (best->estimate * w1 + r.estimate * w2) * (1.0 / (w1 + w2));
        best->uncertaintyCm = std::max(50.0, std::sqrt(1.0 / (w1 + w2 / std::max(confidence, 1e-6))));
        best->confidence = 1.0 - (1.0 - best->confidence) * (1.0 - confidence);
        best->threat = std::max(best->threat, r.threat);
        best->lastTick = std::max(best->lastTick, r.tick);
        ++best->reports;
        best->cleared = false;  // fresh positive evidence
        best->sources |= 1u << sourceSlot;
        return;
    }
    if (m.board.size() >= cfg_.maxContacts) {
        auto victim = std::min_element(m.board.begin(), m.board.end(), [](const Contact& a, const Contact& b) {
            return a.confidence != b.confidence ? a.confidence < b.confidence : a.id < b.id;
        });
        if (victim->confidence >= confidence) return;
        m.board.erase(victim);
    }
    Contact c;
    c.id = m.nextContactId++;
    c.threat = r.threat;
    c.estimate = r.estimate;
    c.uncertaintyCm = std::max(50.0, r.uncertaintyCm);
    c.confidence = confidence;
    c.firstTick = c.lastTick = r.tick;
    c.reports = 1;
    c.sources = 1u << sourceSlot;
    m.board.push_back(c);
}

void SquadCoordinator::Decay(Member& m, double dt) {
    const double f = std::pow(0.5, dt / cfg_.confidenceHalfLifeTicks);
    for (Contact& c : m.board) {
        c.confidence *= f;
        c.uncertaintyCm += 2.0 * dt;  // the source may have moved
    }
    const double forget = cfg_.forgetConfidence;
    m.board.erase(std::remove_if(m.board.begin(), m.board.end(), [forget](const Contact& c) { return c.confidence < forget; }),
                  m.board.end());
}

void SquadCoordinator::ComputeComms(std::uint32_t s, double weatherMul, Tick now) {
    const auto& idx = squadMembers_[s];
    const SquadDef& sq = cfg_.squads[s];
    const std::size_t n = idx.size();
    std::vector<std::size_t> uf(n);
    std::iota(uf.begin(), uf.end(), std::size_t{0});
    auto root = [&uf](std::size_t x) {
        while (uf[x] != x) x = uf[x] = uf[uf[x]];
        return x;
    };
    auto join = [&](std::size_t a, std::size_t b) {
        const std::size_t ra = root(a), rb = root(b);
        if (ra != rb) uf[std::max(ra, rb)] = std::min(ra, rb);
    };
    std::vector<char> radioUp(n, 0);
    std::vector<double> range(n, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        const Member& m = members_[idx[k]];
        if (!m.alive || m.comms == CommsHealth::Destroyed) continue;
        double r = cfg_.roles[m.role].commsRangeCm * weatherMul;
        if (m.comms == CommsHealth::Damaged) {
            r *= cfg_.damagedCommsRangeMul;
            const double roll = HashToUnit(HashCombine(HashCombine(cfg_.seed, m.id), now));
            if (roll < cfg_.damagedDropChance) continue;  // intermittent dropout this cycle
        }
        radioUp[k] = 1;
        range[k] = r;
    }
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = a + 1; b < n; ++b) {
            if (radioUp[a] == 0 || radioUp[b] == 0) continue;
            if (Distance(members_[idx[a]].position, members_[idx[b]].position) <= std::min(range[a], range[b])) join(a, b);
        }
    }
    // A powered relay links everyone within its range (infrastructure-dependent comms).
    if (!sq.relayNode.empty() && world_ != nullptr && world_->RelayOperational(sq.relayNode)) {
        std::size_t first = n;
        for (std::size_t k = 0; k < n; ++k) {
            if (radioUp[k] == 0) continue;
            if (Distance(members_[idx[k]].position, sq.relayPosition) > sq.relayRangeCm * weatherMul) continue;
            if (first == n) first = k;
            else join(first, k);
        }
    }
    // Leader: lowest id with a working radio this cycle.
    std::size_t leader = n;
    for (std::size_t k = 0; k < n; ++k) {
        if (radioUp[k] != 0) {
            leader = k;
            break;
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        Member& m = members_[idx[k]];
        m.component = static_cast<std::uint32_t>(radioUp[k] != 0 ? root(k) : n + k);  // dead radios are singletons
        const bool connected = leader != n && radioUp[k] != 0 && root(k) == root(leader);
        if (connected != m.connected && bus_ != nullptr) {
            WorldEvent e;
            e.type = connected ? WorldEventType::CommsRestored : WorldEventType::CommsLost;
            e.source = WorldSystem::Squad;
            e.position = m.position;
            e.subject = m.id;
            bus_->Publish(e);
        }
        m.connected = connected;
    }
}

TacticalIntent SquadCoordinator::PatrolIntent(Member& m) const {
    TacticalIntent t;
    t.kind = IntentKind::Patrol;
    const auto& route = cfg_.squads[m.squad].members[m.slot].patrol;
    if (!route.empty()) {
        if (Distance(m.position, route[m.patrolIndex % route.size()]) <= cfg_.arrivalRadiusCm) {
            m.patrolIndex = (m.patrolIndex + 1) % route.size();
        }
        t.target = route[m.patrolIndex % route.size()];
    } else {
        t.target = m.position;
    }
    return t;
}

void SquadCoordinator::SetIntent(Member& m, const TacticalIntent& in, Tick now) {
    const TacticalIntent& cur = m.intent;
    const bool changed = cur.kind != in.kind || cur.contactId != in.contactId || cur.fromSquad != in.fromSquad ||
                         Distance(cur.target, in.target) > cfg_.arrivalRadiusCm;
    TacticalIntent next = in;
    next.version = cur.version;
    next.issuedTick = cur.issuedTick;
    if (changed) {
        ++next.version;
        next.issuedTick = now;
        if (bus_ != nullptr && (cur.kind != in.kind || cur.contactId != in.contactId)) {
            WorldEvent e;
            e.type = WorldEventType::AssignmentChanged;
            e.source = WorldSystem::Squad;
            e.position = in.target;
            e.subject = m.id;
            e.aux = static_cast<std::uint32_t>(in.kind);
            bus_->Publish(e);
        }
    }
    m.intent = next;
}

void SquadCoordinator::Assign(std::uint32_t s, Tick now) {
    const auto& idx = squadMembers_[s];
    std::vector<std::size_t> group;
    for (std::size_t i : idx) {
        if (members_[i].alive && members_[i].connected) group.push_back(i);
    }
    // Robots cut off from the leader: keep the last order for a while, then act alone.
    for (std::size_t i : idx) {
        Member& m = members_[i];
        if (!m.alive || m.connected) continue;
        if (m.intent.fromSquad && now - m.lastOrderTick <= cfg_.staleOrderTicks) continue;
        TacticalIntent t;
        t.kind = IntentKind::LocalAutonomous;
        t.fromSquad = false;
        t.target = m.position;
        const Contact* best = nullptr;
        for (const Contact& c : m.board) {
            if (best == nullptr || c.confidence > best->confidence) best = &c;
        }
        if (best != nullptr) {
            t.target = best->estimate;
            t.contactId = best->id;
            t.radiusCm = best->uncertaintyCm;
        }
        SetIntent(m, t, now);
    }
    if (group.empty()) return;

    const Member& leader = members_[group.front()];
    auto workedOn = [&](const Contact& c) {
        return std::any_of(group.begin(), group.end(), [&](std::size_t i) { return members_[i].intent.contactId == c.id; });
    };
    std::vector<const Contact*> contacts;
    for (const Contact& c : leader.board) {
        // Hysteresis: a contact already being worked keeps its team a bit longer.
        const double need = (workedOn(c) && !c.cleared) ? 0.5 * cfg_.investigateMinConfidence : cfg_.investigateMinConfidence;
        if (c.confidence >= need) contacts.push_back(&c);
    }
    std::sort(contacts.begin(), contacts.end(), [this](const Contact* a, const Contact* b) {
        const double sa = a->confidence * cfg_.threatWeights[static_cast<std::size_t>(a->threat)];
        const double sb = b->confidence * cfg_.threatWeights[static_cast<std::size_t>(b->threat)];
        return sa != sb ? sa > sb : a->id < b->id;
    });

    const std::size_t keepPatrol = static_cast<std::size_t>(std::floor(cfg_.minPatrolFraction * static_cast<double>(group.size())));
    const std::size_t maxTasked = group.size() - keepPatrol;
    std::vector<char> tasked(members_.size(), 0);
    std::vector<TacticalIntent> orders(members_.size());
    std::size_t taskedCount = 0;

    auto pick = [&](auto eligible, auto cost) -> std::size_t {
        std::size_t best = members_.size();
        double bestCost = 1e300;
        for (std::size_t i : group) {
            if (tasked[i] != 0 || !eligible(members_[i])) continue;
            const double c = cost(members_[i]);
            if (c < bestCost) {
                bestCost = c;
                best = i;
            }
        }
        return best;
    };

    bool hostile = false;
    for (const Contact* c : contacts) {
        if (taskedCount >= maxTasked) break;
        hostile = hostile || (c->threat == ThreatClass::Hostile && c->confidence >= cfg_.hostileFlankConfidence);
        // 1. Investigator: closest capable robot, sticky to whoever is already on it.
        const std::size_t inv = pick([this](const Member& m) { return cfg_.roles[m.role].canInvestigate; },
                                     [&](const Member& m) {
                                         double cost = Distance(m.position, c->estimate) / cfg_.roles[m.role].investigateWeight;
                                         const bool onIt = (m.intent.kind == IntentKind::Investigate || m.intent.kind == IntentKind::Search) &&
                                                           m.intent.contactId == c->id;
                                         const bool watching = m.intent.kind == IntentKind::Observe && m.intent.contactId == c->id;
                                         if (onIt) cost *= cfg_.assignmentHysteresis;
                                         if (watching) cost /= cfg_.assignmentHysteresis;  // a scout on watch stays on watch
                                         return cost;
                                     });
        if (inv == members_.size()) continue;
        const Member& im = members_[inv];
        const bool alreadySearching = im.intent.kind == IntentKind::Search && im.intent.contactId == c->id;
        TacticalIntent t;
        t.kind = alreadySearching || Distance(im.position, c->estimate) <= std::max(cfg_.arrivalRadiusCm, 0.5 * c->uncertaintyCm)
                     ? IntentKind::Search
                     : IntentKind::Investigate;
        t.target = c->estimate;
        t.radiusCm = c->uncertaintyCm;
        t.contactId = c->id;
        orders[inv] = t;
        tasked[inv] = 1;
        ++taskedCount;
        // 2. Observer: a scout watches from standoff instead of walking in.
        if (taskedCount < maxTasked) {
            const std::size_t obs = pick([this](const Member& m) { return cfg_.roles[m.role].canObserve; },
                                         [&](const Member& m) {
                                             const double d = Distance(m.position, c->estimate);
                                             return (m.intent.kind == IntentKind::Observe && m.intent.contactId == c->id) ? d * cfg_.assignmentHysteresis : d;
                                         });
            if (obs != members_.size()) {
                const Member& o = members_[obs];
                const double standoff = cfg_.roles[o.role].observationStandoffCm;
                Vec3 dir = o.position - c->estimate;
                const double len = dir.Length();
                dir = len > 1.0 ? dir * (1.0 / len) : Vec3{1, 0, 0};
                TacticalIntent ob;
                ob.kind = IntentKind::Observe;
                ob.target = c->estimate + dir * standoff;
                ob.radiusCm = standoff;
                ob.contactId = c->id;
                orders[obs] = ob;
                tasked[obs] = 1;
                ++taskedCount;
            }
        }
        // 3. Flanker on confident hostile contacts.
        if (c->threat == ThreatClass::Hostile && c->confidence >= cfg_.hostileFlankConfidence && taskedCount < maxTasked) {
            const std::size_t fl = pick([this](const Member& m) { return cfg_.roles[m.role].canFlank; },
                                        [&](const Member& m) { return Distance(m.position, c->estimate); });
            if (fl != members_.size()) {
                const Vec3 toContact = c->estimate - members_[inv].position;
                const double len = std::max(1.0, toContact.Length());
                const Vec3 perp{-toContact.y / len, toContact.x / len, 0.0};
                TacticalIntent f;
                f.kind = IntentKind::Flank;
                f.target = c->estimate + perp * (cfg_.roles[members_[fl].role].observationStandoffCm * 0.7);
                f.radiusCm = c->uncertaintyCm;
                f.contactId = c->id;
                orders[fl] = f;
                tasked[fl] = 1;
                ++taskedCount;
            }
        }
    }
    // 4. Defensive positions when a confident hostile is known.
    if (hostile) {
        for (std::size_t i : group) {
            if (taskedCount >= maxTasked) break;
            if (tasked[i] != 0 || !cfg_.roles[members_[i].role].canDefend) continue;
            TacticalIntent d;
            d.kind = IntentKind::Defend;
            d.target = cfg_.squads[s].defendPoint;
            d.radiusCm = 500.0;
            orders[i] = d;
            tasked[i] = 1;
            ++taskedCount;
        }
    }
    for (std::size_t i : group) {
        Member& m = members_[i];
        SetIntent(m, tasked[i] != 0 ? orders[i] : PatrolIntent(m), now);
        m.lastOrderTick = now;
    }
}

void SquadCoordinator::Step(Tick now, double weatherCommsMul) {
    if (!IsValid()) return;
    if (!started_) {
        started_ = true;
        lastStep_ = now;
        nextStep_ = now;
    }
    if (now < nextStep_) return;
    nextStep_ = now + cfg_.coordinationIntervalTicks;
    const double dt = static_cast<double>(now - lastStep_);
    lastStep_ = now;
    const double mul = std::isfinite(weatherCommsMul) ? std::clamp(weatherCommsMul, 0.05, 1.5) : 1.0;

    for (Member& m : members_) Decay(m, dt);
    for (std::uint32_t s = 0; s < squadMembers_.size(); ++s) ComputeComms(s, mul, now);

    // Share matured reports with whoever is reachable from the reporter right now.
    std::vector<Pending> keep;
    for (const Pending& p : pending_) {
        if (p.deliverAt > now) {
            keep.push_back(p);
            continue;
        }
        const Member& from = members_[p.member];
        for (std::size_t i : squadMembers_[from.squad]) {
            Member& to = members_[i];
            if (i == p.member || !to.alive || !from.alive || to.component != from.component) continue;
            Fuse(to, p.report, p.report.confidence * cfg_.sharedConfidenceMul, from.slot);
            ++shared_;
        }
    }
    pending_.swap(keep);

    for (std::uint32_t s = 0; s < squadMembers_.size(); ++s) Assign(s, now);
}

const TacticalIntent* SquadCoordinator::Intent(RobotId robot) const {
    const Member* m = FindMember(robot);
    return m != nullptr ? &m->intent : nullptr;
}

const std::vector<Contact>* SquadCoordinator::KnownContacts(RobotId robot) const {
    const Member* m = FindMember(robot);
    return m != nullptr ? &m->board : nullptr;
}

bool SquadCoordinator::IsConnected(RobotId robot) const {
    const Member* m = FindMember(robot);
    return m != nullptr && m->connected;
}

bool SquadCoordinator::CanCommunicate(RobotId a, RobotId b) const {
    const Member* x = FindMember(a);
    const Member* y = FindMember(b);
    return x != nullptr && y != nullptr && x->squad == y->squad && x->alive && y->alive && x->component == y->component &&
           x->comms != CommsHealth::Destroyed && y->comms != CommsHealth::Destroyed;
}

std::uint32_t SquadCoordinator::SquadOf(RobotId robot) const {
    const Member* m = FindMember(robot);
    return m != nullptr ? m->squad : kInvalidIndex;
}

double SquadCoordinator::ObjectivePriorityFor(RobotId robot) const {
    const Member* m = FindMember(robot);
    if (m == nullptr) return 0.0;
    switch (m->intent.kind) {
        case IntentKind::Investigate:
        case IntentKind::Search:
        case IntentKind::Flank:
        case IntentKind::LocalAutonomous: return 0.0;  // free to act on what it hears
        case IntentKind::Observe:
        case IntentKind::Defend: return 90.0;          // hold the position, only look
        case IntentKind::Patrol: return 55.0;          // only strong local evidence diverts it
    }
    return 0.0;
}

std::uint64_t SquadCoordinator::StateHash() const {
    StateHasher h;
    for (const Member& m : members_) {
        h.Add(m.id);
        h.Add(static_cast<std::uint64_t>(m.intent.kind));
        h.AddVec(m.intent.target);
        h.Add(m.intent.contactId);
        h.Add(m.connected ? 1u : 0u);
        h.Add(m.board.size());
        for (const Contact& c : m.board) {
            h.AddVec(c.estimate);
            h.AddDouble(c.confidence);
            h.Add(c.sources);
        }
    }
    h.Add(pending_.size());
    return h.Value();
}

}  // namespace llworld
