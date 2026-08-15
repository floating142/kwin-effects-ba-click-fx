// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief 定义特效本体与 KCM 共用的配置键、默认值和取值范围。
 *
 * 这里只定义平台相关偏好和功能开关。颜色、拖尾宽度、距离发射间距及辉光参数均由
 * Unity 数据确定，并在子系统参数中完成换算，不作为用户配置项。
 */

#pragma once

namespace baclickfx::defaults
{

/** @name 配置组与配置键
 * @{
 */

/// KWin 配置文件中的特效配置组。
inline constexpr const char *kGroup = "Effect-ba-click-fx";

/// 运行时调试日志开关的配置键。
inline constexpr const char *kDebugLog = "DebugLog";

/// 重绘区域可视化开关的配置键。
inline constexpr const char *kDebugDamage = "DebugDamage";

/// 动画时间倍率的配置键。
inline constexpr const char *kTimeScale = "TimeScale";

/// 特效整体缩放倍率的配置键。
inline constexpr const char *kGlobalScale = "GlobalScale";

/// 拖尾开关的配置键。
inline constexpr const char *kEnableTrail = "EnableTrail";

/// 距离发射器开关的配置键。
inline constexpr const char *kEnableDistanceEmitter = "EnableDistanceEmitter";

/** @} */

/**
 * @name 默认值与取值范围
 *
 * 特效侧限制与配置页控件共用这些范围，确保界面可选值与运行时有效值一致。
 * @{
 */

/// 默认关闭运行时调试日志。
inline constexpr bool kDebugLogDefault = false;

/// 默认关闭重绘区域可视化。
inline constexpr bool kDebugDamageDefault = false;

/// 默认动画时间倍率。
inline constexpr double kTimeScaleDefault = 1.0;

/// 动画时间倍率下限。
inline constexpr double kTimeScaleMin = 0.2;

/// 动画时间倍率上限。
inline constexpr double kTimeScaleMax = 5.0;

/// 默认特效整体缩放倍率。
inline constexpr double kGlobalScaleDefault = 1.0;

/// 特效整体缩放倍率下限。
inline constexpr double kGlobalScaleMin = 0.5;

/// 特效整体缩放倍率上限。
inline constexpr double kGlobalScaleMax = 5.0;

/// 默认启用拖尾。
inline constexpr bool kEnableTrailDefault = true;

/// 默认启用距离发射器。
inline constexpr bool kEnableDistanceEmitterDefault = true;

/** @} */

} // namespace baclickfx::defaults
