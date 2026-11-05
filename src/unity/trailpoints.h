// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace baclickfx::unity {
struct TrailPointRecord {
    std::array<float, 3> position{};
    float time = 0;
    std::uint32_t emitting = 1;
};
static_assert(sizeof(TrailPointRecord) == 20);

// de7330..de7401, autoDestruct disabled as in FX_Touch: keep one old point
// until the next knot expires, so the tail can slide between the two knots.
template<typename TimeAt>
std::size_t expiredTrailPrefix(std::size_t count, float now, float lifetime, TimeAt timeAt)
{
    std::size_t first = 0;
    while (first < count) {
        const std::size_t candidate = first + 1 < count ? first + 1 : first;
        volatile float expires = timeAt(candidate) + lifetime;
        if (!(now > expires)) break;
        ++first;
    }
    return first;
}

// UnityPlayer de6160..de6620. Output order is head -> tail; the stored head is
// replaced by the current transform, and one retained old point clips the tail.
inline void prepareTrailPoints(std::span<const TrailPointRecord> ring, std::size_t first,
    std::size_t count, const std::array<float, 3> &head, bool emitting, float now, float lifetime,
    std::span<std::array<float, 3>> positions, std::span<float> flags)
{
    if (!count) return;
    positions[0] = head;
    flags[0] = emitting ? 1.f : 0.f;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const auto &point = ring[(first + i) % ring.size()];
        positions[count - 1 - i] = point.position;
        flags[count - 1 - i] = point.emitting ? 1.f : 0.f;
    }
    if (count <= 2) return;
    const float before = ring[first % ring.size()].time;
    const float after = ring[(first + 1) % ring.size()].time;
    volatile float elapsed = now - lifetime;
    elapsed -= before;
    volatile float interval = after - before;
    volatile float fraction = elapsed / std::max(float(interval), 9.999999717180685e-10f);
    const float blend = std::min(1.f, std::max(0.f, float(fraction)));
    for (std::size_t axis = 0; axis < 3; ++axis) {
        volatile float delta = positions[count - 2][axis] - positions[count - 1][axis];
        volatile float weighted = delta * blend;
        positions[count - 1][axis] += weighted;
    }
}
}
