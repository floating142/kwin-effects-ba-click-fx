// SPDX-License-Identifier: GPL-3.0-or-later

#include "bloomutils.h"

#include <algorithm>
#include <cmath>

namespace baclickfx
{

namespace
{
// Unity PPv2 的固定 Diffusion 设置和最大金字塔级数。
constexpr int kBloomDiffusion = 7;
constexpr int kBloomMaxLevels = 16;
}

QSize bloomBaseSize(const QSize &devicePixels)
{
    // AnamorphicRatio 为 0 时，PPv2 从输出尺寸的一半开始建立金字塔。
    return QSize(devicePixels.width() / 2, devicePixels.height() / 2);
}

BloomPyramidParams bloomPyramidParams(const QSize &baseSize)
{
    // PPv2：logs = log2(max(width, height)) + min(diffusion, 10) - 10。
    const float longSide = float(std::max(baseSize.width(), baseSize.height()));
    const float logs = std::log2(std::max(longSide, 1.0f))
        + float(std::min(kBloomDiffusion, 10)) - 10.0f;
    const float logsFloor = std::floor(logs);

    BloomPyramidParams params;
    // 至少保留一级，最多遵守 PPv2 的 k_MaxPyramidSize。
    params.iterations = std::clamp(int(logsFloor), 1, kBloomMaxLevels);
    // 保留 logs 的小数部分，避免把上采样尺度量化成整数。
    params.sampleScale = 0.5f + logs - logsFloor;
    return params;
}

double bloomReachPx(const QSize &devicePixels, double scale)
{
    const BloomPyramidParams params = bloomPyramidParams(bloomBaseSize(devicePixels));
    // 设备像素传播半径由最后一级尺度和采样核共同决定，再换算回逻辑像素。
    const double reachDevice =
        (1.0 + double(params.sampleScale)) * double(1 << params.iterations);
    return reachDevice / std::max(scale, 0.01);
}

} // namespace baclickfx
