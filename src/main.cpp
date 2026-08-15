// SPDX-License-Identifier: GPL-3.0-or-later
// KWin 插件入口。工厂宏嵌入 metadata.json，并将 supported() 接入加载判定。

#include "baclickfxeffect.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(BaClickFxEffect,
                              "metadata.json",
                              return BaClickFxEffect::supported();)

} // namespace KWin

#include "main.moc"
