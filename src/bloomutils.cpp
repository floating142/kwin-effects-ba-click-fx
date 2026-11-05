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
    // 每次采样的双线性足迹也占半个源纹理像素。降采样四点核贡献 1.5，
    // 上采样（含最终合成）贡献 sampleScale/2 + 0.5，不能只估计最粗一级。
    // 使用实际纹理尺寸计算，覆盖奇数分辨率、窄屏及短边已降到 1 的金字塔。
    double reachDevice = 1.5; // 全分辨率到第 0 级的预筛选。
    QSize level = bloomBaseSize(devicePixels).expandedTo(QSize(1, 1));
    for (int i = 0; i < params.iterations; ++i) {
        const double texel = std::max(double(devicePixels.width()) / level.width(),
                                      double(devicePixels.height()) / level.height());
        reachDevice += (double(params.sampleScale) * .5 + .5) * texel;
        if (i + 1 < params.iterations) {
            reachDevice += 1.5 * texel;
        }
        level = QSize(std::max(1, level.width() / 2), std::max(1, level.height() / 2));
    }
    return reachDevice / std::max(scale, 0.01);
}

} // namespace baclickfx
