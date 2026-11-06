// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <effect/effect.h>

#include <chrono>

namespace KWin
{

class RenderDevice;

/// 以 KWin 6.8 为默认入口，将旧版本 API 转换为特效共用的生命周期。
/// 调用链的转发和失败传播集中在这里，子类只负责更新状态及绘制叠层。
class BaClickFxEffectBase : public Effect
{
public:
    BaClickFxEffectBase();
#ifndef BA_CLICK_FX_KWIN_LEGACY_PREPAINT
    void prePaintScreen(ScreenPrePaintData &data) final;
    void pointerMotion(PointerMotionEvent *event) final;
#else
    void prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime) final;
#endif

#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    [[nodiscard]] bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                   int mask, const Region &deviceRegion, LogicalOutput *screen) final;
#else
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                     int mask, const Region &deviceRegion, LogicalOutput *screen) final;
#endif

protected:
    virtual void prepareScreen(ScreenPrePaintData &data, std::chrono::nanoseconds presentTime) = 0;
    virtual void paintOverlay(const RenderTarget &renderTarget, const RenderViewport &viewport,
                              const Region &deviceRegion, LogicalOutput *screen) = 0;
    virtual void handlePointerMotion(const QPointF &position) = 0;
    virtual void releaseRenderDevice(const RenderDevice *device) = 0;
    const RenderDevice *currentRenderDevice() const;

private:
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    RenderDevice *m_viewRenderDevice = nullptr;
#endif
};

}
