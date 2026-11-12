// LAST LIGHT - Acoustic Intelligence System (AIS)
// Core value types shared by every AIS module. Engine-agnostic C++17.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ais {

// Deterministic simulation time. AIS never reads wall-clock time.
using Tick = std::uint64_t;
using EventId = std::uint64_t;
using EntityId = std::uint32_t;
inline constexpr EntityId kNoEntity = 0;

// Units follow Unreal: centimetres, +Z up.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3() = default;
    constexpr Vec3(double ax, double ay, double az) : x(ax), y(ay), z(az) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr double LengthSq() const { return Dot(*this); }
    double Length() const { return std::sqrt(LengthSq()); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

inline double DistanceSq(const Vec3& a, const Vec3& b) { return (a - b).LengthSq(); }
inline double Distance(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

// Coarse spectral bands. Low frequencies pass through walls; high frequencies
// carry localisation detail and are absorbed by air, rain and foliage.
enum class Band : std::uint8_t { Low = 0, Mid = 1, High = 2 };
inline constexpr std::size_t kBandCount = 3;
using BandArray = std::array<double, kBandCount>;

inline constexpr double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline constexpr double Clamp01(double v) { return Clamp(v, 0.0, 1.0); }
inline constexpr double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// Energetic sum of two levels in dB.
inline double PowerSumDb(double aDb, double bDb) {
    const double hi = aDb > bDb ? aDb : bDb;
    const double lo = aDb > bDb ? bDb : aDb;
    return hi + 10.0 * std::log10(1.0 + std::pow(10.0, (lo - hi) / 10.0));
}

// SplitMix64 finaliser: portable, integer-only, identical on every platform.
inline constexpr std::uint64_t Mix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline constexpr std::uint64_t HashCombine(std::uint64_t seed, std::uint64_t v) {
    return Mix64(seed ^ (v + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)));
}
// Uniform double in [0, 1) from 53 hash bits.
inline double HashToUnit(std::uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t Next() {
        state_ += 0x9E3779B97F4A7C15ull;
        return Mix64(state_);
    }
    double NextUnit() { return HashToUnit(Next()); }
    double Range(double lo, double hi) { return lo + (hi - lo) * NextUnit(); }

private:
    std::uint64_t state_;
};

// FNV-1a accumulator used for replay/state comparison.
class StateHasher {
public:
    void Add(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h_ ^= (v >> (i * 8)) & 0xFFull;
            h_ *= 1099511628211ull;
        }
    }
    // Quantised so that -0.0/0.0 and sub-quantum noise do not differ.
    void AddDouble(double v, double quantum = 1e-6) {
        Add(static_cast<std::uint64_t>(std::llround(v / quantum)));
    }
    void AddVec(const Vec3& v) {
        AddDouble(v.x);
        AddDouble(v.y);
        AddDouble(v.z);
    }
    std::uint64_t Value() const { return h_; }

private:
    std::uint64_t h_ = 1469598103934665603ull;
};

}  // namespace ais
