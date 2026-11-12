// LAST LIGHT - Container interaction framework implementation.
#include "llloot/ContainerService.h"

#include <algorithm>

namespace llloot {

namespace {
constexpr const char* kResultNames[kLootResultCodeCount] = {
    "Accepted",          "Started",          "RejectedNotAuthority", "RejectedRaidInactive", "RejectedUnknownPlayer",
    "RejectedDuplicate", "RejectedUnknownContainer", "RejectedOutOfRange", "RejectedDestroyed", "RejectedHidden",
    "RejectedObstructed", "RejectedNoPower", "RejectedMissingTool", "RejectedNoClearance", "RejectedSealed",
    "RejectedBusy",      "RejectedAlreadySearched", "RejectedNotSearched", "RejectedNotOwner", "RejectedInvalidSlot",
    "RejectedSlotTaken", "RejectedInventory", "RejectedNotApplicable",
};
constexpr const char* kEventNames[9] = {"AccessChanged",   "Discovered", "ObstructionCleared", "SearchStarted", "SearchInterrupted",
                                        "SearchCompleted", "ItemTaken",  "ItemReserved",       "ItemReturned"};

bool Has(const ContainerDef& d, AccessKind k) {
    return std::any_of(d.access.begin(), d.access.end(), [k](const AccessRequirement& a) { return a.kind == k; });
}
}  // namespace

const char* ToString(LootResultCode c) {
    const auto i = static_cast<std::size_t>(c);
    return i < kLootResultCodeCount ? kResultNames[i] : "?";
}
const char* ToString(LootEventType t) {
    const auto i = static_cast<std::size_t>(t);
    return i < 9 ? kEventNames[i] : "?";
}

ContainerService::ContainerService(const LootConfig& config, const IItemCatalog& catalog, ContainerServiceConfig sc)
    : cfg_(config), catalog_(catalog), sc_(sc) {
    // Sorted node -> containers index: built in O(n log n), queried by binary search.
    std::vector<std::pair<std::string, std::uint32_t>> pairs;
    for (std::size_t c = 0; c < cfg_.containers.size(); ++c) {
        for (const AccessRequirement& a : cfg_.containers[c].access) {
            if (a.kind == AccessKind::Power) pairs.emplace_back(a.ref, static_cast<std::uint32_t>(c));
        }
    }
    std::sort(pairs.begin(), pairs.end());
    for (const auto& [node, c] : pairs) {
        if (powerIndex_.empty() || powerIndex_.back().first != node) powerIndex_.push_back({node, {}});
        powerIndex_.back().second.push_back(c);
    }
    rt_.resize(cfg_.containers.size());
}

void ContainerService::BeginRaid(std::uint64_t raidSeed) {
    raidSeed_ = raidSeed;
    active_ = true;
    work_.clear();
    pending_.clear();
    rt_.assign(cfg_.containers.size(), Runtime{});
    for (std::size_t c = 0; c < rt_.size(); ++c) {
        if (cfg_.OwnershipFor(cfg_.containers[c]) == OwnershipPolicy::Shared) rt_[c].instances.emplace_back();
        rt_[c].lastAccessible = PowerOk(static_cast<std::uint32_t>(c));
    }
    // Players who left during the previous raid free their slot; connected players stay.
    players_.erase(std::remove_if(players_.begin(), players_.end(), [](const Player& p) { return !p.connected; }), players_.end());
    for (Player& p : players_) {
        p.recentCount = 0;
        p.recentHead = 0;
    }
}

void ContainerService::EndRaid() {
    active_ = false;
    work_.clear();
}

// ---------------------------------------------------------------- players

ContainerService::Player* ContainerService::FindPlayer(PlayerId id) {
    for (Player& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}
const ContainerService::Player* ContainerService::FindPlayer(PlayerId id) const {
    for (const Player& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool ContainerService::ConnectPlayer(PlayerId id, std::uint64_t key, const Vec3& position) {
    if (id == 0 || !position.IsFinite()) return false;
    if (Player* p = FindPlayer(id)) {
        if (p->key != key) return false;  // a reconnect must keep its recipient key
        p->connected = true;
        p->recentCount = 0;  // sequence numbers are per session; replays are still caught by state checks
        p->recentHead = 0;
        p->position = position;
        return true;
    }
    if (players_.size() >= sc_.maxPlayers || std::any_of(players_.begin(), players_.end(), [key](const Player& q) { return q.key == key; })) {
        // Reconnect under a new engine id: the same party slot (key) rebinds if
        // its previous session is disconnected. A live session cannot be hijacked.
        for (Player& q : players_) {
            if (q.key != key) continue;
            if (q.connected) return false;
            const PlayerId old = q.id;
            q.id = id;
            q.connected = true;
            q.position = position;
            q.recentCount = 0;
            q.recentHead = 0;
            for (Runtime& r : rt_) {
                for (Instance& inst : r.instances) {
                    if (inst.owner == old) inst.owner = id;
                    for (PlayerId& h : inst.holders) h = h == old ? id : h;
                }
            }
            for (PendingGrant& g : pending_) {
                if (g.owner == old) g.owner = id;
            }
            return true;
        }
        return false;
    }
    Player p;
    p.id = id;
    p.key = key;
    p.connected = true;
    p.position = position;
    players_.push_back(p);
    return true;
}

void ContainerService::DisconnectPlayer(PlayerId id) {
    Player* p = FindPlayer(id);
    if (p == nullptr) return;
    p->connected = false;
    for (std::size_t i = 0; i < work_.size(); ++i) {
        if (work_[i].player == id) {
            Interrupt(i);
            break;
        }
    }
}

bool ContainerService::UpdatePlayerPosition(PlayerId id, const Vec3& position) {
    Player* p = FindPlayer(id);
    if (p == nullptr || !position.IsFinite()) return false;
    p->position = position;
    return true;
}

bool ContainerService::SeenSequence(Player& p, std::uint32_t seq) {
    for (std::size_t i = 0; i < p.recentCount; ++i) {
        if (p.recent[i] == seq) return true;
    }
    p.recent[p.recentHead] = seq;
    p.recentHead = (p.recentHead + 1) % p.recent.size();
    p.recentCount = std::min(p.recentCount + 1, p.recent.size());
    return false;
}

bool ContainerService::InRange(const Player& p, std::uint32_t c) const {
    const ContainerDef& d = cfg_.containers[c];
    const std::uint32_t t = cfg_.FindContainerType(d.type);
    const double range = (t == kNone ? 200.0 : cfg_.containerTypes[t].interactRangeCm) + sc_.rangeToleranceCm;
    return llworld::Distance(p.position, d.position) <= range;
}

// ---------------------------------------------------------------- instances

ContainerService::Instance* ContainerService::InstanceFor(std::uint32_t c, PlayerId viewer) {
    Runtime& r = rt_[c];
    if (cfg_.OwnershipFor(cfg_.containers[c]) == OwnershipPolicy::Shared) return r.instances.empty() ? nullptr : &r.instances[0];
    for (Instance& i : r.instances) {
        if (i.owner == viewer) return &i;
    }
    if (FindPlayer(viewer) == nullptr || r.instances.size() >= sc_.maxPlayers) return nullptr;
    r.instances.emplace_back();
    r.instances.back().owner = viewer;
    return &r.instances.back();
}

const ContainerService::Instance* ContainerService::InstanceFor(std::uint32_t c, PlayerId viewer) const {
    if (c >= rt_.size()) return nullptr;
    const Runtime& r = rt_[c];
    if (cfg_.OwnershipFor(cfg_.containers[c]) == OwnershipPolicy::Shared) return r.instances.empty() ? nullptr : &r.instances[0];
    for (const Instance& i : r.instances) {
        if (i.owner == viewer) return &i;
    }
    return nullptr;
}

void ContainerService::EnsureGenerated(std::uint32_t c, Instance& inst) {
    if (inst.generated) return;
    std::uint64_t key = 0;
    if (inst.owner != 0) {
        const Player* p = FindPlayer(inst.owner);
        key = p != nullptr ? p->key : inst.owner;
    }
    inst.result = GenerateLoot(cfg_, catalog_, cfg_.containers[c], ContainerSeed(raidSeed_, cfg_.containers[c].id, key));
    inst.slots.assign(inst.result.items.size(), SlotState::Available);
    inst.holders.assign(inst.result.items.size(), 0);
    inst.generated = true;
    ++stats_.generated;
}

ContainerService::Work* ContainerService::WorkOf(PlayerId p) {
    for (Work& w : work_) {
        if (w.player == p) return &w;
    }
    return nullptr;
}

bool ContainerService::UsesTool(std::uint32_t c) const { return Has(cfg_.containers[c], AccessKind::Tool) && !rt_[c].lockReleased; }

bool ContainerService::PowerOk(std::uint32_t c) const {
    if (rt_[c].lockReleased) return true;
    for (const AccessRequirement& a : cfg_.containers[c].access) {
        if (a.kind == AccessKind::Power && (access_ == nullptr || !access_->IsPowered(a.ref))) return false;
    }
    return true;
}

LootResultCode ContainerService::CheckAccess(std::uint32_t c, PlayerId player) const {
    if (c >= cfg_.containers.size()) return LootResultCode::RejectedUnknownContainer;
    const ContainerDef& d = cfg_.containers[c];
    const Runtime& r = rt_[c];
    if (d.condition == ContainerCondition::Destroyed) return LootResultCode::RejectedDestroyed;
    // Order matters: report the requirement the player has to solve first.
    for (AccessKind k : {AccessKind::Discovery, AccessKind::Work, AccessKind::WorldSeal, AccessKind::Power, AccessKind::Clearance, AccessKind::Tool}) {
        for (const AccessRequirement& a : d.access) {
            if (a.kind != k) continue;
            switch (k) {
                case AccessKind::Discovery:
                    if (!r.discovered) return LootResultCode::RejectedHidden;
                    break;
                case AccessKind::Work:
                    if (!r.cleared) return LootResultCode::RejectedObstructed;
                    break;
                case AccessKind::WorldSeal:
                    if (access_ != nullptr && access_->IsSealed(a.ref)) return LootResultCode::RejectedSealed;
                    break;
                case AccessKind::Power:
                    if (!r.lockReleased && (access_ == nullptr || !access_->IsPowered(a.ref))) return LootResultCode::RejectedNoPower;
                    break;
                case AccessKind::Clearance:
                    if (access_ == nullptr || !access_->HasClearance(player, a.ref)) return LootResultCode::RejectedNoClearance;
                    break;
                case AccessKind::Tool:
                    if (!r.lockReleased && (access_ == nullptr || !access_->HasItem(player, a.ref))) return LootResultCode::RejectedMissingTool;
                    break;
            }
        }
    }
    return LootResultCode::Accepted;
}

// ---------------------------------------------------------------- requests

LootResultCode ContainerService::Record(LootResultCode c) {
    ++stats_.results[static_cast<std::size_t>(c)];
    return c;
}

LootResultCode ContainerService::Submit(const LootRequest& req) {
    if (!sc_.serverAuthority) return Record(LootResultCode::RejectedNotAuthority);
    if (!active_) return Record(LootResultCode::RejectedRaidInactive);
    Player* p = FindPlayer(req.player);
    if (p == nullptr || !p->connected) return Record(LootResultCode::RejectedUnknownPlayer);
    if (req.sequence == 0 || SeenSequence(*p, req.sequence)) return Record(LootResultCode::RejectedDuplicate);
    if (req.action == LootAction::CancelSearch) {
        for (std::size_t i = 0; i < work_.size(); ++i) {
            if (work_[i].player == p->id) {
                Interrupt(i);
                return Record(LootResultCode::Accepted);
            }
        }
        return Record(LootResultCode::RejectedNotApplicable);
    }
    if (req.container >= cfg_.containers.size()) return Record(LootResultCode::RejectedUnknownContainer);
    const std::uint32_t c = req.container;
    switch (req.action) {
        case LootAction::Search: return Record(StartSearch(*p, c));
        case LootAction::ClearObstruction: return Record(StartClear(*p, c));
        case LootAction::TakeItem: return Record(TakeItem(*p, c, req.slot));
        case LootAction::Discover: {
            if (!Has(cfg_.containers[c], AccessKind::Discovery)) return Record(LootResultCode::RejectedNotApplicable);
            if (!InRange(*p, c)) return Record(LootResultCode::RejectedOutOfRange);
            if (!rt_[c].discovered) {
                rt_[c].discovered = true;
                Emit(LootEventType::Discovered, c, p->id);
            }
            return Record(LootResultCode::Accepted);
        }
        case LootAction::CancelSearch: break;
    }
    return Record(LootResultCode::RejectedNotApplicable);
}

LootResultCode ContainerService::StartSearch(Player& p, std::uint32_t c) {
    if (!InRange(p, c)) return LootResultCode::RejectedOutOfRange;
    const LootResultCode access = CheckAccess(c, p.id);
    if (access != LootResultCode::Accepted) return access;
    if (WorkOf(p.id) != nullptr) return LootResultCode::RejectedBusy;
    Instance* inst = InstanceFor(c, p.id);
    if (inst == nullptr) return LootResultCode::RejectedNotOwner;
    if (inst->state == InstanceState::Searched) return LootResultCode::RejectedAlreadySearched;
    if (inst->state == InstanceState::Searching) return LootResultCode::RejectedBusy;  // claim held by someone else
    // Atomic claim: requests are processed serially on the server, so the first
    // request to reach this line owns the container until it completes or is interrupted.
    inst->state = InstanceState::Searching;
    inst->claimant = p.id;
    const ContainerDef& d = cfg_.containers[c];
    const ContainerTypeDef& t = cfg_.containerTypes[cfg_.FindContainerType(d.type)];
    const bool forcing = UsesTool(c);
    if (Has(d, AccessKind::Power)) rt_[c].lockReleased = true;  // powered lock opens and stays open
    work_.push_back(Work{p.id, c, WorkKind::Search, now_ + t.searchTicks + (forcing ? t.forceTicks : 0)});
    ++stats_.searchesStarted;
    Emit(LootEventType::SearchStarted, c, p.id);
    if (forcing) Sound(LootSoundKind::ForceLock, c, p.id, t.forceNoiseDb, t.forceTicks);
    Sound(LootSoundKind::Search, c, p.id, t.searchNoiseDb, t.searchTicks);
    return LootResultCode::Started;
}

LootResultCode ContainerService::StartClear(Player& p, std::uint32_t c) {
    const ContainerDef& d = cfg_.containers[c];
    auto it = std::find_if(d.access.begin(), d.access.end(), [](const AccessRequirement& a) { return a.kind == AccessKind::Work; });
    if (it == d.access.end() || rt_[c].cleared) return LootResultCode::RejectedNotApplicable;
    if (!InRange(p, c)) return LootResultCode::RejectedOutOfRange;
    if (d.condition == ContainerCondition::Destroyed) return LootResultCode::RejectedDestroyed;
    if (WorkOf(p.id) != nullptr) return LootResultCode::RejectedBusy;
    for (const Work& w : work_) {
        if (w.container == c && w.kind == WorkKind::Clear) return LootResultCode::RejectedBusy;
    }
    work_.push_back(Work{p.id, c, WorkKind::Clear, now_ + it->workTicks});
    Sound(LootSoundKind::ClearDebris, c, p.id, 70.0, it->workTicks);
    return LootResultCode::Started;
}

LootResultCode ContainerService::TakeItem(Player& p, std::uint32_t c, std::uint32_t slot) {
    Instance* inst = cfg_.OwnershipFor(cfg_.containers[c]) == OwnershipPolicy::Shared ? InstanceFor(c, p.id) : nullptr;
    if (inst == nullptr) {
        for (Instance& i : rt_[c].instances) {
            if (i.owner == p.id) inst = &i;
        }
    }
    if (inst == nullptr || inst->state != InstanceState::Searched) return LootResultCode::RejectedNotSearched;
    if (!InRange(p, c)) return LootResultCode::RejectedOutOfRange;
    if (slot >= inst->slots.size()) return LootResultCode::RejectedInvalidSlot;
    if (inst->slots[slot] != SlotState::Available) return LootResultCode::RejectedSlotTaken;
    if (grants_ == nullptr) return LootResultCode::RejectedInventory;
    const LootItem& item = inst->result.items[slot];
    PendingGrant g;
    g.container = c;
    g.owner = inst->owner;
    g.slot = slot;
    g.grant.grantId = llworld::HashCombine(inst->result.seed, 0xA11CEull + slot);
    g.grant.player = p.id;
    g.grant.containerId = cfg_.containers[c].id;
    g.grant.slot = slot;
    g.grant.itemId = item.itemId;
    g.grant.quantity = item.quantity;
    g.grant.condition = item.condition;
    inst->slots[slot] = SlotState::Reserved;  // reserve before calling out: no re-entrant double take
    inst->holders[slot] = p.id;
    bool keep = false;
    const GrantOutcome o = grants_->Grant(g.grant);
    ApplyGrantOutcome(g, o, &keep);
    if (keep) {
        pending_.push_back(g);
        return LootResultCode::Started;
    }
    return o == GrantOutcome::Rejected ? LootResultCode::RejectedInventory : LootResultCode::Accepted;
}

void ContainerService::ApplyGrantOutcome(PendingGrant& g, GrantOutcome o, bool* keepPending) {
    Instance* inst = nullptr;
    for (Instance& i : rt_[g.container].instances) {
        if (i.owner == g.owner) inst = &i;
    }
    *keepPending = false;
    if (inst == nullptr) return;
    switch (o) {
        case GrantOutcome::Accepted:
        case GrantOutcome::AlreadyApplied:
            inst->slots[g.slot] = SlotState::Taken;
            ++stats_.itemsGranted;
            Emit(LootEventType::ItemTaken, g.container, g.grant.player, g.slot);
            break;
        case GrantOutcome::Rejected:
            inst->slots[g.slot] = SlotState::Available;
            inst->holders[g.slot] = 0;
            Emit(LootEventType::ItemReturned, g.container, g.grant.player, g.slot);
            break;
        case GrantOutcome::Unavailable:
            *keepPending = true;
            Emit(LootEventType::ItemReserved, g.container, g.grant.player, g.slot);
            break;
    }
}

// ---------------------------------------------------------------- simulation

void ContainerService::Interrupt(std::size_t i) {
    const Work w = work_[i];
    work_.erase(work_.begin() + static_cast<std::ptrdiff_t>(i));
    if (w.kind == WorkKind::Search) {
        for (Instance& inst : rt_[w.container].instances) {
            if (inst.claimant == w.player && inst.state == InstanceState::Searching) {
                // Nothing was revealed; the next search regenerates identical contents.
                inst.state = InstanceState::Unsearched;
                inst.claimant = 0;
            }
        }
        ++stats_.searchesInterrupted;
    }
    Emit(LootEventType::SearchInterrupted, w.container, w.player, w.kind == WorkKind::Search ? 0u : 1u);
}

void ContainerService::Complete(const Work& w) {
    if (w.kind == WorkKind::Clear) {
        rt_[w.container].cleared = true;
        Emit(LootEventType::ObstructionCleared, w.container, w.player);
        return;
    }
    for (Instance& inst : rt_[w.container].instances) {
        if (inst.claimant != w.player || inst.state != InstanceState::Searching) continue;
        EnsureGenerated(w.container, inst);
        inst.state = InstanceState::Searched;
        inst.claimant = 0;
        if (Has(cfg_.containers[w.container], AccessKind::Tool)) rt_[w.container].lockReleased = true;
        ++stats_.searchesCompleted;
        Emit(LootEventType::SearchCompleted, w.container, w.player, static_cast<std::uint32_t>(inst.result.items.size()));
        return;
    }
}

void ContainerService::Step(Tick now) {
    now_ = now;
    if (!active_) return;
    for (std::size_t i = 0; i < work_.size();) {
        const Work w = work_[i];
        const Player* p = FindPlayer(w.player);
        if (p == nullptr || !p->connected || !InRange(*p, w.container)) {
            Interrupt(i);
            continue;
        }
        if (w.done <= now_) {
            work_.erase(work_.begin() + static_cast<std::ptrdiff_t>(i));
            Complete(w);
            continue;
        }
        ++i;
    }
    if (grants_ == nullptr) return;
    std::size_t budget = sc_.maxGrantRetriesPerStep;
    for (std::size_t i = 0; i < pending_.size() && budget > 0; --budget) {
        bool keep = false;
        ++stats_.grantRetries;
        ApplyGrantOutcome(pending_[i], grants_->Grant(pending_[i].grant), &keep);
        if (keep) {
            ++i;
        } else {
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void ContainerService::NotifyInfrastructureChanged(std::string_view node) {
    if (!active_) return;
    for (const auto& [name, list] : powerIndex_) {
        if (name != node) continue;
        for (std::uint32_t c : list) {
            const bool ok = PowerOk(c);
            if (ok != rt_[c].lastAccessible) {
                rt_[c].lastAccessible = ok;
                Emit(LootEventType::AccessChanged, c, 0, ok ? 1u : 0u);
            }
        }
    }
}

void ContainerService::Emit(LootEventType t, std::uint32_t c, PlayerId p, std::uint32_t aux) {
    if (observer_ != nullptr) observer_->OnLootEvent(LootEvent{t, c, p, now_, aux});
}

void ContainerService::Sound(LootSoundKind k, std::uint32_t c, PlayerId p, double db, Tick duration) {
    if (sound_ == nullptr) return;
    LootSound s;
    s.kind = k;
    s.container = c;
    s.player = p;
    s.position = cfg_.containers[c].position;
    s.loudnessDb = db;
    s.durationTicks = std::max<Tick>(1, duration);
    s.tick = now_;
    sound_->OnLootSound(s);
}

// ---------------------------------------------------------------- queries

InstanceState ContainerService::StateFor(std::uint32_t c, PlayerId viewer) const {
    const Instance* i = InstanceFor(c, viewer);
    return i == nullptr ? InstanceState::Unsearched : i->state;
}

const LootResult* ContainerService::Contents(std::uint32_t c, PlayerId viewer) const {
    const Instance* i = InstanceFor(c, viewer);
    return (i != nullptr && i->state == InstanceState::Searched) ? &i->result : nullptr;
}

SlotView ContainerService::Slot(std::uint32_t c, PlayerId viewer, std::uint32_t slot) const {
    SlotView v;
    const Instance* i = InstanceFor(c, viewer);
    if (i == nullptr || i->state != InstanceState::Searched || slot >= i->slots.size()) return v;
    v.item = &i->result.items[slot];
    v.state = i->slots[slot];
    v.holder = i->holders[slot];
    return v;
}

PlayerId ContainerService::Claimant(std::uint32_t c, PlayerId viewer) const {
    const Instance* i = InstanceFor(c, viewer);
    return i == nullptr ? 0 : i->claimant;
}

std::size_t ContainerService::PendingGrants() const { return pending_.size(); }

std::uint64_t ContainerService::StateHash() const {
    llworld::StateHasher h;
    h.Add(raidSeed_);
    h.Add(now_);
    for (const Runtime& r : rt_) {
        h.Add((r.discovered ? 1u : 0u) | (r.cleared ? 2u : 0u) | (r.lockReleased ? 4u : 0u));
        for (const Instance& i : r.instances) {
            h.Add(i.owner);
            h.Add(static_cast<std::uint64_t>(i.state));
            h.Add(i.claimant);
            h.Add(i.generated ? i.result.Hash() : 0);
            for (std::size_t s = 0; s < i.slots.size(); ++s) {
                h.Add(static_cast<std::uint64_t>(i.slots[s]));
                h.Add(i.holders[s]);
            }
        }
    }
    for (const Work& w : work_) {
        h.Add(w.player);
        h.Add(w.container);
        h.Add(w.done);
    }
    return h.Value();
}

}  // namespace llloot
