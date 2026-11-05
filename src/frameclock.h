// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>

namespace baclickfx {

/// 只接受光标所在输出的呈现帧；时间戳决定节奏，不另设 FPS 上限。
class OutputFrameClock
{
public:
    struct Tick {
        bool accepted = false;
        std::chrono::nanoseconds elapsed{};
    };

    Tick take(std::uintptr_t output, std::uintptr_t driver, std::chrono::nanoseconds now)
    {
        if (driver && output != driver) return {};
        // 换屏后的预测呈现时间也可能落后。不能把这种零时间帧当成采样帧，
        // 否则 updateDrag 会移动发射器起点，却因 dt=0 跳过这段距离的粒子。
        if (m_started && now <= m_lastSimulation) return {};
        Tick tick{true, {}};
        if (m_started && now > m_lastSimulation) tick.elapsed = now - m_lastSimulation;
        if (!m_started || now > m_lastSimulation) m_lastSimulation = now;
        m_started = true;
        return tick;
    }

    void reset() { *this = {}; }

private:
    bool m_started = false;
    std::chrono::nanoseconds m_lastSimulation{};
};
}
