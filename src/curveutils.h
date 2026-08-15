// SPDX-License-Identifier: GPL-3.0-or-later
// 通用数值、颜色空间与 Unity 曲线采样工具。

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace baclickfx {

/**
 * Unity 世界单位在 1080 像素参考高度下对应的逻辑像素数。
 *
 * 固定 UI Pass 使用 `Matrix4x4.Ortho(-aspect, aspect, -1, 1, ...)`，即
 * `orthographicSize = 1.0`。因此参考高度下一个世界单位对应 540 像素。
 * 实际输出换算由 `Subsystem::worldUnitPx` 再乘 `outputHeight / 1080`，最终保持
 * 一个世界单位等于输出逻辑高度的一半。
 */
inline constexpr double kUnitySizeToPx = 540.0;

/// Unity 参考分辨率的逻辑高度，用于按输出高度归一化尺寸。
inline constexpr double kReferenceHeightPx = 1080.0;

/**
 * 使用 IEC 61966-2-1 分段函数将单个 sRGB 分量解码到线性空间。
 *
 * @param c sRGB 编码分量。
 * @return 线性空间分量；非正输入返回 0。
 *
 * Unity 工程使用 Linear 颜色空间，Inspector 中保存的 RGB 值必须先解码再混合。
 * Alpha 始终按线性量处理，不调用此函数。
 */
inline double srgbToLinearChannel(double c)
{
    if (c <= 0.0) {
        return 0.0;
    }
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

using Rgb = std::array<double, 3>;

/// 标量曲线关键帧；NaN 切线表示采样时回退到线性插值。
struct ScalarStop {
    double t = 0.0;
    double v = 0.0;
    double inSlope = std::numeric_limits<double>::quiet_NaN();
    double outSlope = std::numeric_limits<double>::quiet_NaN();
};

/// RGB 颜色曲线关键帧。
struct ColorStop {
    double t = 0.0;
    Rgb c{0.0, 0.0, 0.0};
};

/// 不拥有数据的只读关键帧视图。
template<typename T>
struct StopSpan {
    const T *data = nullptr;
    std::size_t size = 0;

    constexpr StopSpan() = default;
    constexpr StopSpan(const T *d, std::size_t n)
        : data(d)
        , size(n)
    {
    }
    template<std::size_t N>
    constexpr StopSpan(const T (&arr)[N])
        : data(arr)
        , size(N)
    {
    }
    template<std::size_t N>
    constexpr StopSpan(const std::array<T, N> &arr)
        : data(arr.data())
        , size(N)
    {
    }
    StopSpan(const std::vector<T> &v)
        : data(v.data())
        , size(v.size())
    {
    }

    constexpr bool empty() const { return size == 0; }
    constexpr const T &operator[](std::size_t i) const { return data[i]; }
    constexpr const T &front() const { return data[0]; }
    constexpr const T &back() const { return data[size - 1]; }
};

using ScalarCurve = StopSpan<ScalarStop>;
using ColorCurve = StopSpan<ColorStop>;

/// 将数值限制在闭区间 `[min, max]`。
double clamp(double v, double min, double max);

/// 在两个标量之间执行线性插值。
double lerp(double a, double b, double t);

/// 计算归一化时间的二次缓出值。
double easeOutQuad(double t);

/// 将弧度角归一化到 `[0, 2π)`。
double normalizeAngle(double a);

/**
 * 采样标量曲线。
 *
 * 有有效切线时使用 Unity 风格 Hermite 插值，否则使用线性插值；空曲线返回
 * `fallback`。
 */
double evalScalarStops(const ScalarCurve &stops, double t, double fallback = 1.0);

/// 逐通道线性采样 RGB 曲线；空曲线返回 `fallback`。
Rgb evalColorStops(const ColorCurve &stops, double t, const Rgb &fallback);

/**
 * 粒子发射使用的均匀随机数源。
 *
 * 显式种子构造函数用于生成可复现序列；默认构造函数用于运行时随机播种。
 */
class Rng
{
public:
    explicit Rng(std::uint64_t seed);
    /// 使用系统随机设备播种。
    Rng();

    /// 返回 `[a, b)` 内的均匀随机数；`a == b` 时返回 `a`。
    double range(double a, double b);

private:
    std::mt19937_64 m_engine;
};

} // namespace baclickfx
