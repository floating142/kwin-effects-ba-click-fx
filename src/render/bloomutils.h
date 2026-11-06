// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSize>

namespace baclickfx
{

/// PPv2 Bloom 金字塔的尺寸参数。
/// `iterations` 是降采样级数，`sampleScale` 是上采样阶段使用的连续采样尺度。
struct BloomPyramidParams
{
    int iterations = 1;
    float sampleScale = 1.0f;
};

/// 从单个输出的设备像素尺寸计算半分辨率的第 0 级尺寸。
QSize bloomBaseSize(const QSize &devicePixels);

/// 按 Unity PPv2 的 Diffusion 公式计算级数和采样尺度。
/// `baseSize` 必须是 `bloomBaseSize()` 的结果，不能传虚拟桌面并集。
BloomPyramidParams bloomPyramidParams(const QSize &baseSize);

/// 计算 Bloom 在逻辑像素中的最大传播半径，用于外扩 damage 区域。
/// `scale` 是输出缩放；非正或过小值会使用内部保护下限。
double bloomReachPx(const QSize &devicePixels, double scale);

} // namespace baclickfx
