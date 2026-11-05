// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: fx/particles/ShittimParticleUv.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleUv.h
 * @brief 计算源 Grid / Lifetime / WholeSheet 纹理动画坐标。
 */
#include "initial.h"
#include <cmath>

namespace baclickfx::unity {
// 当前 UnityPlayer 的 Grid/Lifetime/WholeSheet Constant/TwoConstants 路径；Curve、SingleRow、FPS、Speed 和 Sprite 模式需要单独移植。
struct TextureSheet {
    bool enabled=false;
    Range frame,start;
    float normalizedFrame(std::uint32_t particleSeed) const {
        if(!enabled)return 0;
        Random startRng(particleSeed+0x56b3dbb0u);
        Random frameRng(particleSeed+0x13740583u);
        const float first=start.sample(startRng.value());
        const float animation=frame.sample(frameRng.value());
        // 源执行 addps，再 floor/subps；不能改成双精度或两阶段取模，图块边界舍入可能选中另一帧。
        volatile float sum=animation+first;
        return sum-std::floor(sum);
    }
};
inline int textureSheetFrame(float normalized,int columns,int rows) {
    volatile float scaled=normalized*float(columns*rows);
    return std::clamp(int(scaled),0,columns*rows-1);
}
}
