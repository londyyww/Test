// LAST LIGHT - Environmental & World Intelligence (WI)
// Environmental hazards: gas, fire, smoke, electrical, structural, chemical
// and machinery. Gameplay approximations driven by data, infrastructure state,
// weather and world events. Propagation only follows designer-placed sites
// (no unbounded chain reactions).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llworld/Ini.h"
#include "llworld/Weather.h"
#include "llworld/WorldEvents.h"
#include "llworld/WorldTypes.h"

namespace llworld {

enum class HazardType : std::uint8_t { GasLeak = 0, Fire, Smoke, Electrical, Structural, Chemical, Machinery };
inline constexpr std::size_t kHazardTypeCount = 7;
const char* ToString(HazardType t);
bool ParseHazardType(std::string_view s, HazardType& out);

// Dormant: inactive/stable. Active: dangerous. Suppressed: a dependency
// (power) is missing. Spent: finished (burned out, closed, collapsed).
enum class HazardState : std::uint8_t { Dormant = 0, Active, Suppressed, Spent };
const char* ToString(HazardState s);

enum class HazardPersistence : std::uint8_t { Mission, Facility };

enum class HazardSound : std::uint8_t {
    FireIgnite, FireCrackle, GasHiss, GasIgnition, ElectricalArc, StructureCreak, StructureCollapse, MachineryGrind
};

enum class HazardAction : std::uint8_t { Extinguish, CloseValve, Trigger };

struct HazardDef {
    std::string id;
    HazardType type = HazardType::Fire;
    Vec3 position;
    double radiusCm = 300.0;
    double maxRadiusCm = 0.0;  // chemical growth cap; 0 = radius
    double severity = 0.5;     // 0..1, scales intensity and damage
    bool startsActive = false;
    Tick durationTicks = 0;    // fire fuel / leak duration; 0 = until resolved

    // Propagation
    bool flammable = false;         // fire sites only: may be ignited by spread
    double spreadRadiusCm = 0.0;
    Tick spreadIntervalTicks = 150;
    double spreadChance = 0.3;
    double growthPerSecond = 0.0;   // gas concentration / chemical radius (cm)

    // Dependencies
    std::string powerNode;          // Electrical: live when energized. Machinery: when running.
    bool exposed = true;            // affected by rain / wind

    // Interaction rules
    double ignitionThreshold = 0.5; // gas concentration needed to ignite
    std::string ignites;            // gas -> fire site activated by ignition
    std::string smoke;              // fire -> smoke hazard driven by it
    std::string alarm;              // power-grid alarm triggered on activation
    std::string extinguishItem;     // fire: needs this item (consumed)
    bool playerTriggerable = false; // e.g. opening a gas valve, starting a crusher
    double interactRangeCm = 250.0;
    std::uint32_t routeId = 0;      // route blocked while active / when collapsed
    std::vector<std::string> damagesNodes;
    double nodeDamagePerSecond = 0.0;  // while burning
    double burstNodeDamage = 0.0;      // gas ignition / collapse

    // Effects on players and sensors
    double damagePerSecond = 0.0;
    double burstDamage = 0.0;          // instant, on ignition/collapse
    double visibilityReduction = 0.0;  // smoke: 0..1
    double thermalClutter = 0.0;       // fire heat confuses thermal sensors
    double movementPenalty = 0.0;
    double activationNoiseDb = 0.0;
    double noiseDb = 0.0;
    Tick noiseIntervalTicks = 0;

    // Structural
    double stressWarn = 0.5;
    double stressCollapse = 1.0;
    double stressPerLoudEvent = 0.25;
    double stressPerFireSecond = 0.02;
    double stressPerWindSecond = 0.0;

    HazardPersistence persistence = HazardPersistence::Mission;
};

struct HazardConfig {
    std::vector<HazardDef> hazards;
    double ticksPerSecond = 30.0;
    Tick updateIntervalTicks = 15;
    Tick exposureIntervalTicks = 15;
    double wetElectricalRadiusMul = 0.6;  // wet ground extends electrical danger
    double smokeDissipateSeconds = 30.0;
    std::size_t maxPlayers = 4;
    double requestBurst = 4.0;
    double requestRefillPerTick = 0.2;
    std::size_t causeHistory = 128;
    std::uint64_t seed = 0x48415A44ull;
};

std::vector<std::string> ValidateHazards(const HazardConfig& c);
// Sections: [hazards] and [hazard.<ID>].
void LoadHazards(const IniDocument& doc, HazardConfig& c, std::vector<std::string>& errors);

// Port to infrastructure, inventory and acoustics. Implemented in
// integration/ over the power grid and AIS; tests use fakes.
class IHazardWorld {
public:
    virtual ~IHazardWorld() = default;
    virtual int ResolveNode(std::string_view id) const = 0;  // -1 = unknown
    virtual bool NodeEnergized(int node) const = 0;
    virtual bool NodeRunning(int node) const = 0;
    virtual void DamageNode(int node, double amount) = 0;
    virtual void TriggerAlarm(int node) = 0;
    virtual void EmitSound(HazardSound sound, const Vec3& position, double loudnessDb, Tick durationTicks, std::uint32_t hazard) = 0;
    virtual bool PlayerHasItem(PlayerId player, std::string_view item) const = 0;
    virtual bool ConsumeItem(PlayerId player, std::string_view item) = 0;
};

struct HazardRuntime {
    HazardState state = HazardState::Dormant;
    double intensity = 0.0;  // fire heat / gas concentration / smoke density
    double radius = 0.0;
    double stress = 0.0;
    double fuel = 0.0;       // remaining fire fuel in ticks (< 0 = endless)
    Tick activatedTick = 0;
    Tick nextSpread = 0;
    Tick nextNoise = 0;
    bool collapsed = false;
    bool burnedOut = false;
    bool escalated = false;
    bool routeBlocked = false;
    std::uint32_t version = 1;
    bool dirty = true;
};

struct HazardExposure {
    double damagePerSecond = 0.0;
    double visibilityReduction = 0.0;
    double thermalClutter = 0.0;
    double movementPenalty = 0.0;
    std::uint32_t typesMask = 0;  // bit per HazardType present
};

struct HazardRequest {
    PlayerId player = 0;
    std::uint32_t sequence = 0;
    std::uint32_t hazard = kInvalidIndex;
    HazardAction action = HazardAction::Extinguish;
    Vec3 playerPosition;
};

enum class HazardRequestResult : std::uint8_t {
    Applied, RejectedDuplicate, RejectedRateLimited, RejectedUnknown, RejectedOutOfRange, RejectedInvalidState,
    RejectedMissingItem, RejectedUnsupported, RejectedTooManyPlayers
};
const char* ToString(HazardRequestResult r);

struct HazardReplicationState {
    std::uint32_t hazard = kInvalidIndex;
    std::uint8_t state = 0;
    std::uint8_t intensity = 0;  // 0..255
    std::uint16_t radiusDm = 0;  // decimetres
    std::uint8_t flags = 0;      // 1 collapsed, 2 burned out, 4 route blocked, 8 escalated
    std::uint32_t version = 0;
};

struct HazardDamageRecord {
    std::string id;
    bool collapsed = false;
    bool burnedOut = false;
    double stress = 0.0;
};

struct PlayerSample {
    PlayerId player = 0;
    Vec3 position;
};

class HazardSystem {
public:
    HazardSystem(const HazardConfig& config, WorldEventBus* bus, IHazardWorld* world);

    bool IsValid() const { return errors_.empty(); }
    const std::vector<std::string>& Errors() const { return errors_; }

    // Server-side activation (scripts, damage system, world events). `causeId`
    // de-duplicates the same cause reported twice (0 = no dedup).
    bool Activate(std::uint32_t hazard, std::uint64_t causeId);
    HazardRequestResult Interact(const HazardRequest& request, Tick now);
    void NotifyInfrastructureChanged() { dependencyDirty_ = true; }

    void Step(Tick now, const WeatherSnapshot& weather);
    // Samples players at the exposure interval; publishes PlayerExposure events.
    void SamplePlayers(const std::vector<PlayerSample>& players, Tick now);

    HazardExposure ExposureAt(const Vec3& position) const;
    bool IsRouteBlocked(std::uint32_t routeId) const;
    std::uint32_t Find(std::string_view id) const;
    const HazardDef& Def(std::uint32_t i) const { return cfg_.hazards[i]; }
    const HazardRuntime& State(std::uint32_t i) const { return rt_[i]; }
    std::size_t Count() const { return rt_.size(); }
    std::size_t ActiveCount() const { return active_.size(); }
    double DoseOf(PlayerId player) const;

    void CollectReplication(std::vector<HazardReplicationState>& out);
    std::vector<HazardDamageRecord> ExportFacilityDamage() const;
    bool ImportFacilityDamage(const std::vector<HazardDamageRecord>& records);  // before first Step only

    std::uint64_t StateHash() const;
    std::uint64_t Updates() const { return updates_; }

private:
    struct Burst {
        Vec3 position;
        double radius;
        double damage;
    };
    struct PlayerSlot {
        PlayerId id = 0;
        double tokens = 0.0;
        Tick stamp = 0;
        std::array<std::uint32_t, 32> recent{};
        std::size_t count = 0;
        std::size_t head = 0;
        double dose = 0.0;
    };

    void Changed(std::uint32_t i);
    void Publish(WorldEventType type, std::uint32_t i, double magnitude = 0.0, double radius = 0.0, std::uint32_t aux = 0);
    void SetState(std::uint32_t i, HazardState s);
    bool ActivateInternal(std::uint32_t i, Tick now);
    void Ignite(std::uint32_t gas, Tick now);
    void Collapse(std::uint32_t i, Tick now);
    void UpdateHazard(std::uint32_t i, Tick now, double dtTicks, const WeatherSnapshot& w);
    void ApplyLoudEvent(const WorldEvent& e);
    bool IsIgnitionSource(std::uint32_t i) const;
    PlayerSlot* SlotFor(PlayerId p);

    HazardConfig cfg_;
    WorldEventBus* bus_;
    IHazardWorld* world_;
    std::vector<std::string> errors_;
    std::vector<HazardRuntime> rt_;
    std::vector<int> nodeOf_;                       // resolved powerNode
    std::vector<std::vector<int>> damageNodes_;     // resolved damagesNodes
    std::vector<int> alarmOf_;
    std::vector<std::uint32_t> ignitesOf_, smokeOf_, parentFire_;
    std::vector<std::vector<std::uint32_t>> spreadTo_, ignitionSources_, nearbyFires_;
    std::vector<std::uint32_t> active_;      // sorted
    std::vector<std::uint32_t> structures_;  // always monitored
    std::vector<std::pair<std::uint32_t, std::uint64_t>> causes_;
    std::size_t causeHead_ = 0;
    std::vector<Burst> bursts_;
    std::vector<PlayerSlot> players_;
    Tick now_ = 0;
    Tick nextUpdate_ = 0;
    Tick nextExposure_ = 0;
    bool started_ = false;
    bool dependencyDirty_ = true;
    std::uint32_t lastWeatherVersion_ = 0xFFFFFFFFu;
    WeatherSnapshot weather_;
    std::uint64_t updates_ = 0;
};

}  // namespace llworld
