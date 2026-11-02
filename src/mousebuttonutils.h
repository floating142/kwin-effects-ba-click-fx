// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtCore/Qt>

namespace baclickfx
{

inline constexpr Qt::MouseButtons kSupportedTriggerButtons =
    Qt::LeftButton | Qt::RightButton | Qt::MiddleButton;

constexpr Qt::MouseButtons normalizedTriggerButtons(int buttons)
{
    return Qt::MouseButtons::fromInt(buttons) & kSupportedTriggerButtons;
}

struct MouseButtonTransitions {
    Qt::MouseButtons pressed;
    bool wasHeld;
    bool held;

    bool startsGesture() const { return !wasHeld && held; }
    bool endsGesture() const { return wasHeld && !held; }
};

/// 点击按每个新按键触发，拖尾则由整组已选按键共同维持。
inline MouseButtonTransitions mouseButtonTransitions(Qt::MouseButtons buttons,
                                                      Qt::MouseButtons oldButtons,
                                                      Qt::MouseButtons triggers)
{
    return {(buttons & ~oldButtons) & triggers,
            bool(oldButtons & triggers), bool(buttons & triggers)};
}

}
