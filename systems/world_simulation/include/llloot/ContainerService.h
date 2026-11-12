// LAST LIGHT - Container interaction framework (server-authoritative).
//
// Owns container search state for one raid: access requirements, atomic search
// claims, duplicate-request rejection, deterministic recovery and per-item
// hand-off. It never stores what a player owns. Items move into a player's
// possession only via ILootGrantPort, keyed by a stable grant id so a retried
// or duplicated request can never be applied twice.
//
// Power, tools, clearances and lockdowns are answered by ILootAccessPort, so
// this module does not duplicate the power grid, inventory or world simulation.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llloot/Loot.h"

namespace llloot {

// Answers access questions from the systems that own the answer.
class ILootAccessPort {
public:
    virtual ~ILootAccessPort() = default;
    virtual bool IsPowered(std::string_view infrastructureNode) const = 0;         // power grid
    virtual bool HasItem(PlayerId player, std::string_view itemId) const = 0;      // authoritative inventory
    virtual bool HasClearance(PlayerId player, std::string_view tag) const = 0;    // mission / inventory
    virtual bool IsSealed(std::string_view worldEntity) const = 0;                 // world simulation lockdown
};

struct LootGrant {
    std::uint64_t grantId = 0;  // stable: raid seed + container + recipient + slot
    PlayerId player = 0;
    std::string containerId;
    std::uint32_t slot = 0;
    std::string itemId;
    std::uint32_t quantity = 0;
    ItemCondition condition;
};

enum class GrantOutcome : std::uint8_t {
    Accepted,        // applied now
    AlreadyApplied,  // this grantId was applied before (retry after a lost response)
    Rejected,        // permanent refusal (no space): the item stays in the container
    Unavailable,     // transient failure: the item stays reserved and is retried
};

// Implemented by the authoritative inventory service. MUST be idempotent per grantId.
class ILootGrantPort {
public:
    virtual ~ILootGrantPort() = default;
    virtual GrantOutcome Grant(const LootGrant& grant) = 0;
};

enum class LootSoundKind : std::uint8_t { Search, ForceLock, ClearDebris };

struct LootSound {
    LootSoundKind kind = LootSoundKind::Search;
    std::uint32_t container = kNone;
    PlayerId player = 0;
    Vec3 position;
    double loudnessDb = 0.0;
    Tick durationTicks = 1;
    Tick tick = 0;
};

// Acoustic Intelligence (or any listener) receives searches as sounds.
class ILootSoundSink {
public:
    virtual ~ILootSoundSink() = default;
    virtual void OnLootSound(const LootSound& s) = 0;
};

enum class LootEventType : std::uint8_t {
    AccessChanged,     // aux = 1 accessible / 0 blocked (power-gated containers)
    Discovered,
    ObstructionCleared,
    SearchStarted,
    SearchInterrupted,
    SearchCompleted,   // aux = number of items
    ItemTaken,         // aux = slot
    ItemReserved,      // grant pending (inventory unavailable), aux = slot
    ItemReturned,      // grant rejected, aux = slot
};

struct LootEvent {
    LootEventType type = LootEventType::SearchStarted;
    std::uint32_t container = kNone;
    PlayerId player = 0;
    Tick tick = 0;
    std::uint32_t aux = 0;
};

class ILootObserver {
public:
    virtual ~ILootObserver() = default;
    virtual void OnLootEvent(const LootEvent& e) = 0;
};

enum class LootAction : std::uint8_t { Search, CancelSearch, Discover, ClearObstruction, TakeItem };

struct LootRequest {
    PlayerId player = 0;
    std::uint32_t sequence = 0;  // per-player, client-generated; 0 is invalid
    std::uint32_t container = kNone;
    LootAction action = LootAction::Search;
    std::uint32_t slot = 0;  // TakeItem
};

enum class LootResultCode : std::uint8_t {
    Accepted = 0,
    Started,
    RejectedNotAuthority,
    RejectedRaidInactive,
    RejectedUnknownPlayer,
    RejectedDuplicate,
    RejectedUnknownContainer,
    RejectedOutOfRange,
    RejectedDestroyed,
    RejectedHidden,
    RejectedObstructed,
    RejectedNoPower,
    RejectedMissingTool,
    RejectedNoClearance,
    RejectedSealed,
    RejectedBusy,             // another player holds the claim / player already busy
    RejectedAlreadySearched,
    RejectedNotSearched,
    RejectedNotOwner,
    RejectedInvalidSlot,
    RejectedSlotTaken,
    RejectedInventory,
    RejectedNotApplicable,
};
inline constexpr std::size_t kLootResultCodeCount = 23;
const char* ToString(LootResultCode c);
const char* ToString(LootEventType t);

enum class InstanceState : std::uint8_t { Unsearched, Searching, Searched };
enum class SlotState : std::uint8_t { Available, Reserved, Taken };

struct ContainerServiceConfig {
    std::uint32_t maxPlayers = 4;
    std::size_t maxGrantRetriesPerStep = 8;
    double rangeToleranceCm = 50.0;  // network jitter allowance on top of interact range
    bool serverAuthority = true;     // false on clients: every request is rejected
};

struct SlotView {
    const LootItem* item = nullptr;
    SlotState state = SlotState::Available;
    PlayerId holder = 0;
};

class ContainerService {
public:
    ContainerService(const LootConfig& config, const IItemCatalog& catalog, ContainerServiceConfig sc = {});

    void SetAccessPort(ILootAccessPort* p) { access_ = p; }
    void SetGrantPort(ILootGrantPort* p) { grants_ = p; }
    void SetSoundSink(ILootSoundSink* s) { sound_ = s; }
    void SetObserver(ILootObserver* o) { observer_ = o; }

    // Raid lifecycle. BeginRaid discards all previous raid state.
    void BeginRaid(std::uint64_t raidSeed);
    void EndRaid();
    bool RaidActive() const { return active_; }
    std::uint64_t RaidSeed() const { return raidSeed_; }

    // Players (1..maxPlayers). recipientKey seeds PerPlayer instances; use a
    // stable per-raid key (e.g. party slot) so reconnects see the same loot.
    bool ConnectPlayer(PlayerId player, std::uint64_t recipientKey, const Vec3& position);
    void DisconnectPlayer(PlayerId player);  // interrupts its active work
    bool UpdatePlayerPosition(PlayerId player, const Vec3& position);

    LootResultCode Submit(const LootRequest& request);

    // Server tick: completes timed work, interrupts out-of-range work, retries pending grants.
    void Step(Tick now);
    Tick Now() const { return now_; }

    // A power-grid node changed; only containers gated by that node are re-evaluated.
    void NotifyInfrastructureChanged(std::string_view node);

    // Queries -----------------------------------------------------------------
    std::uint32_t Find(std::string_view id) const { return cfg_.FindContainer(id); }
    std::size_t Count() const { return cfg_.containers.size(); }
    const ContainerDef& Def(std::uint32_t c) const { return cfg_.containers[c]; }
    InstanceState StateFor(std::uint32_t c, PlayerId viewer) const;
    bool IsDiscovered(std::uint32_t c) const { return c < rt_.size() && rt_[c].discovered; }
    bool IsObstructionCleared(std::uint32_t c) const { return c < rt_.size() && rt_[c].cleared; }
    // First unmet access requirement for `player` (Accepted when accessible).
    LootResultCode CheckAccess(std::uint32_t c, PlayerId player) const;
    // Contents visible to `viewer`; nullptr until that viewer's instance was searched.
    const LootResult* Contents(std::uint32_t c, PlayerId viewer) const;
    SlotView Slot(std::uint32_t c, PlayerId viewer, std::uint32_t slot) const;
    PlayerId Claimant(std::uint32_t c, PlayerId viewer) const;
    std::size_t PendingGrants() const;

    struct Stats {
        std::uint64_t generated = 0;
        std::uint64_t searchesStarted = 0;
        std::uint64_t searchesCompleted = 0;
        std::uint64_t searchesInterrupted = 0;
        std::uint64_t itemsGranted = 0;
        std::uint64_t grantRetries = 0;
        std::array<std::uint64_t, kLootResultCodeCount> results{};
    };
    const Stats& GetStats() const { return stats_; }
    std::uint64_t StateHash() const;

private:
    struct Instance {
        PlayerId owner = 0;  // 0 = shared instance
        InstanceState state = InstanceState::Unsearched;
        PlayerId claimant = 0;
        bool generated = false;
        LootResult result;
        std::vector<SlotState> slots;
        std::vector<PlayerId> holders;
    };
    struct Runtime {
        bool discovered = false;
        bool cleared = false;
        bool lockReleased = false;  // powered lock opened once: stays open
        bool lastAccessible = false;
        std::vector<Instance> instances;  // shared: one; PerPlayer: one per player
    };
    enum class WorkKind : std::uint8_t { Search, Clear };
    struct Work {
        PlayerId player = 0;
        std::uint32_t container = kNone;
        WorkKind kind = WorkKind::Search;
        Tick done = 0;
    };
    struct Player {
        PlayerId id = 0;
        bool connected = false;
        std::uint64_t key = 0;
        Vec3 position;
        std::array<std::uint32_t, 64> recent{};
        std::size_t recentCount = 0;
        std::size_t recentHead = 0;
    };
    struct PendingGrant {
        std::uint32_t container = kNone;
        PlayerId owner = 0;
        std::uint32_t slot = 0;
        LootGrant grant;
    };

    LootResultCode Record(LootResultCode c);
    Player* FindPlayer(PlayerId p);
    const Player* FindPlayer(PlayerId p) const;
    bool SeenSequence(Player& p, std::uint32_t seq);
    bool InRange(const Player& p, std::uint32_t c) const;
    Instance* InstanceFor(std::uint32_t c, PlayerId viewer);
    const Instance* InstanceFor(std::uint32_t c, PlayerId viewer) const;
    Work* WorkOf(PlayerId p);
    bool UsesTool(std::uint32_t c) const;
    void EnsureGenerated(std::uint32_t c, Instance& inst);
    LootResultCode StartSearch(Player& p, std::uint32_t c);
    LootResultCode StartClear(Player& p, std::uint32_t c);
    LootResultCode TakeItem(Player& p, std::uint32_t c, std::uint32_t slot);
    void Interrupt(std::size_t workIndex);
    void Complete(const Work& w);
    void ApplyGrantOutcome(PendingGrant& g, GrantOutcome o, bool* keepPending);
    void Emit(LootEventType t, std::uint32_t c, PlayerId p, std::uint32_t aux = 0);
    void Sound(LootSoundKind k, std::uint32_t c, PlayerId p, double db, Tick duration);
    bool PowerOk(std::uint32_t c) const;

    const LootConfig& cfg_;
    const IItemCatalog& catalog_;
    ContainerServiceConfig sc_;
    ILootAccessPort* access_ = nullptr;
    ILootGrantPort* grants_ = nullptr;
    ILootSoundSink* sound_ = nullptr;
    ILootObserver* observer_ = nullptr;
    bool active_ = false;
    std::uint64_t raidSeed_ = 0;
    Tick now_ = 0;
    std::vector<Runtime> rt_;
    std::vector<Player> players_;
    std::vector<Work> work_;  // at most one per player
    std::vector<PendingGrant> pending_;
    // Power node -> gated containers (built once; no per-event scan of all containers).
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> powerIndex_;
    Stats stats_;
};

}  // namespace llloot
