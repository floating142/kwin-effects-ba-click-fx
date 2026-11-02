// SPDX-License-Identifier: GPL-3.0-or-later
#include "kwincompat.h"

#include <effect/effecthandler.h>
#include <input_event.h>
#include <scene/scene.h>

namespace KWin
{

#ifndef BA_CLICK_FX_KWIN_LEGACY_PREPAINT
void BaClickFxEffectBase::prePaintScreen(ScreenPrePaintData &data)
{
    const auto now = data.view ? data.view->nextPresentationTimestamp() : std::chrono::nanoseconds{0};
    prepareScreen(data, now);
    effects->prePaintScreen(data);
}

void BaClickFxEffectBase::pointerMotion(PointerMotionEvent *event)
{
    handlePointerMotion(event->position);
}
#else
// KWin 6.6 的时间戳由参数传入，也没有 Effect::pointerMotion()。
// 保留 mouseChanged 和帧级采样回退；不额外合成输入事件。
void BaClickFxEffectBase::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    prepareScreen(data, std::chrono::duration_cast<std::chrono::nanoseconds>(presentTime));
    effects->prePaintScreen(data, presentTime);
}
#endif

#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
bool BaClickFxEffectBase::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                     int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // 底层绘制失败时立即停止，禁止继续提交叠层命令。
    if (!effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen)) {
        return false;
    }
    paintOverlay(renderTarget, viewport, deviceRegion);
    return true;
}
#else
// KWin 6.6/6.7 的绘制调用没有返回值。
void BaClickFxEffectBase::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                     int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    paintOverlay(renderTarget, viewport, deviceRegion);
}
#endif

}
