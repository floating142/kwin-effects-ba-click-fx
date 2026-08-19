// SPDX-License-Identifier: GPL-3.0-or-later

#include "diagnostics.h"

namespace baclickfx
{

QString formatFrameStats(const FrameStatsSummary &s)
{
    return QStringLiteral(
               "event=frame_stats frames=%1 cpu_avg_ms=%2 cpu_max_ms=%3 "
               "cpu_prepaint_avg_ms=%4 cpu_setup_avg_ms=%5 cpu_trail_avg_ms=%6 "
               "cpu_particle_avg_ms=%7 cpu_finish_avg_ms=%8 input_total_ms=%9 "
               "gpu_samples=%10 gpu_avg_ms=%11 gpu_max_ms=%12 gpu_import_avg_ms=%13 "
               "gpu_particle_avg_ms=%14 gpu_bloom_avg_ms=%15 gpu_composite_avg_ms=%16 "
               "frame_interval_avg_ms=%17 frame_interval_max_ms=%18 "
               "device_avg_mpx=%19 device_avg_rects=%20 request_avg_mpx=%21 "
               "request_avg_rects=%22 bloom_source_avg_mpx=%23 bloom_source_avg_rects=%24")
        .arg(s.frames)
        .arg(s.cpuAvgMs, 0, 'f', 3)
        .arg(s.cpuMaxMs, 0, 'f', 3)
        .arg(s.prePaintAvgMs, 0, 'f', 3)
        .arg(s.setupAvgMs, 0, 'f', 3)
        .arg(s.trailAvgMs, 0, 'f', 3)
        .arg(s.particleAvgMs, 0, 'f', 3)
        .arg(s.finishAvgMs, 0, 'f', 3)
        .arg(s.inputTotalMs, 0, 'f', 3)
        .arg(s.gpuSamples)
        .arg(s.gpuAvgMs, 0, 'f', 3)
        .arg(s.gpuMaxMs, 0, 'f', 3)
        .arg(s.gpuImportAvgMs, 0, 'f', 3)
        .arg(s.gpuParticleAvgMs, 0, 'f', 3)
        .arg(s.gpuBloomAvgMs, 0, 'f', 3)
        .arg(s.gpuCompositeAvgMs, 0, 'f', 3)
        .arg(s.frameIntervalAvgMs, 0, 'f', 3)
        .arg(s.frameIntervalMaxMs, 0, 'f', 3)
        .arg(s.deviceAvgMpx, 0, 'f', 3)
        .arg(s.deviceAvgRects, 0, 'f', 2)
        .arg(s.requestAvgMpx, 0, 'f', 3)
        .arg(s.requestAvgRects, 0, 'f', 2)
        .arg(s.bloomSourceAvgMpx, 0, 'f', 3)
        .arg(s.bloomSourceAvgRects, 0, 'f', 2)
        + QStringLiteral(" input_events=%1 input_events_per_frame=%2 input_accepted=%3 input_merged=%4 "
                         "input_discarded=%5 input_cross_screen=%6")
              .arg(s.inputEvents)
              .arg(s.frames > 0 ? double(s.inputEvents) / s.frames : 0.0, 0, 'f', 2)
              .arg(s.inputAccepted)
              .arg(s.inputMerged)
              .arg(s.inputDiscarded)
              .arg(s.inputCrossScreen)
        + QStringLiteral(" output=\"%1\" output_gpu_import_ms=%2 output_gpu_bloom_ms=%3 "
                         "output_gpu_composite_ms=%4 output_device_mpx=%5 output_request_mpx=%6 "
                         "output_source_mpx=%7 output_device_rects=%8 output_request_rects=%9 "
                         "output_source_rects=%10")
              .arg(s.output)
              .arg(s.outputGpuImportMs, 0, 'f', 3)
              .arg(s.outputGpuBloomMs, 0, 'f', 3)
              .arg(s.outputGpuCompositeMs, 0, 'f', 3)
              .arg(s.outputDeviceMpx, 0, 'f', 3)
              .arg(s.outputRequestMpx, 0, 'f', 3)
              .arg(s.outputSourceMpx, 0, 'f', 3)
              .arg(s.outputDeviceRects, 0, 'f', 2)
              .arg(s.outputRequestRects, 0, 'f', 2)
              .arg(s.outputSourceRects, 0, 'f', 2)
        + QStringLiteral(" mouse_changed_events=%1 mouse_changed_motion=%2 "
                         "pointer_motion_events=%3 fallback_samples=%4 no_motion_samples=%5")
              .arg(s.mouseChangedEvents)
              .arg(s.mouseChangedMotion)
              .arg(s.pointerMotionEvents)
              .arg(s.fallbackSamples)
              .arg(s.inputDiscarded)
        + QStringLiteral(" skip_no_activity=%1 skip_no_damage=%2 skip_gpu=%3 "
                         "skip_target=%4 skip_import=%5")
              .arg(s.skipNoActivity)
              .arg(s.skipNoDamage)
              .arg(s.skipGpu)
              .arg(s.skipTarget)
              .arg(s.skipImport);
}

} // namespace baclickfx
