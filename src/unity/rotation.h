// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "curve.h"
#include "random.h"

namespace baclickfx::unity {
// UnityPlayer b05fd0 -> ad5b60: scale each curve before interpolating them.
// The mix and flip streams are derived from the particle seed independently.
template<typename Keys>
float rotationSpeed(const Keys &minimum, const Keys &maximum, float scalar,
                    float progress, std::uint32_t seed, float flipProbability = 0)
{
    Random mix(seed + 0x6aed452eu), flip(seed + 0xff2bb1a4u);
    const float unit = mix.value();
    volatile float low = evaluateCurve(minimum, progress) * scalar;
    volatile float high = evaluateCurve(maximum, progress) * scalar;
    volatile float difference = high - low;
    volatile float weighted = difference * unit;
    const float speed = low + weighted;
    return (flipProbability < flip.value() ? 1.f : -1.f) * speed;
}
}
