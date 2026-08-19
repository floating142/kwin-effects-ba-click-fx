// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <cstdint>

namespace baclickfx
{

struct FrameStatsSummary
{
    int frames = 0;
    int gpuSamples = 0;
    double cpuAvgMs = 0.0;
    double cpuMaxMs = 0.0;
    double prePaintAvgMs = 0.0;
    double setupAvgMs = 0.0;
    double trailAvgMs = 0.0;
    double particleAvgMs = 0.0;
    double finishAvgMs = 0.0;
    double inputTotalMs = 0.0;
    double gpuAvgMs = -1.0;
    double gpuMaxMs = -1.0;
    double gpuImportAvgMs = -1.0;
    double gpuParticleAvgMs = -1.0;
    double gpuBloomAvgMs = -1.0;
    double gpuCompositeAvgMs = -1.0;
    double frameIntervalAvgMs = 0.0;
    double frameIntervalMaxMs = 0.0;
    double deviceAvgMpx = 0.0;
    double requestAvgMpx = 0.0;
    double bloomSourceAvgMpx = 0.0;
    double deviceAvgRects = 0.0;
    double requestAvgRects = 0.0;
    double bloomSourceAvgRects = 0.0;
    std::uint64_t inputEvents = 0;
    std::uint64_t inputAccepted = 0;
    std::uint64_t inputMerged = 0;
    std::uint64_t inputDiscarded = 0;
    std::uint64_t inputCrossScreen = 0;
    std::uint64_t mouseChangedEvents = 0;
    std::uint64_t mouseChangedMotion = 0;
    std::uint64_t pointerMotionEvents = 0;
    std::uint64_t fallbackSamples = 0;
    QString output;
    double outputGpuImportMs = -1.0;
    double outputGpuBloomMs = -1.0;
    double outputGpuCompositeMs = -1.0;
    double outputDeviceMpx = 0.0;
    double outputRequestMpx = 0.0;
    double outputSourceMpx = 0.0;
    double outputDeviceRects = 0.0;
    double outputRequestRects = 0.0;
    double outputSourceRects = 0.0;
    std::uint64_t skipNoActivity = 0;
    std::uint64_t skipNoDamage = 0;
    std::uint64_t skipGpu = 0;
    std::uint64_t skipTarget = 0;
    std::uint64_t skipImport = 0;
};

QString formatFrameStats(const FrameStatsSummary &stats);

} // namespace baclickfx
