// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
// Core value types. Engine-agnostic C++17, no dependency on other LAST LIGHT
// modules (the Acoustic Intelligence bridge lives in bridge/).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace llenv {

using Tick = std::uint64_t;
using NodeIndex = std::uint32_t;  // dense runtime index into the facility
using PlayerId = std::uint32_t;   // server-assigned, 0 = invalid
inline constexpr NodeIndex kInvalidNode = 0xFFFFFFFFu;
inline constexpr PlayerId kNoPlayer = 0;

// Centimetres, +Z up (Unreal convention).
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    constexpr Vec3() = default;
    constexpr Vec3(double ax, double ay, double az) : x(ax), y(ay), z(az) {}
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    double Length() const { return std::sqrt(x * x + y * y + z * z); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};
inline double Distance(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

inline constexpr std::uint64_t Mix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline constexpr std::uint64_t HashCombine(std::uint64_t seed, std::uint64_t v) {
    return Mix64(seed ^ (v + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)));
}
inline double HashToUnit(std::uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

// FNV-1a; doubles are quantised so the hash is stable for replay checks.
class StateHasher {
public:
    void Add(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h_ ^= (v >> (i * 8)) & 0xFFull;
            h_ *= 1099511628211ull;
        }
    }
    void AddDouble(double v) { Add(static_cast<std::uint64_t>(std::llround(v * 1e6))); }
    std::uint64_t Value() const { return h_; }

private:
    std::uint64_t h_ = 1469598103934665603ull;
};

}  // namespace llenv
