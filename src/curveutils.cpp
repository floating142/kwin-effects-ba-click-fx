// SPDX-License-Identifier: GPL-3.0-or-later

#include "curveutils.h"

#include <algorithm>
#include <cmath>

namespace baclickfx {

double clamp(double v, double min, double max)
{
    return std::min(max, std::max(min, v));
}

double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

double easeOutQuad(double t)
{
    return 1 - (1 - t) * (1 - t);
}

double normalizeAngle(double a)
{
    const double twoPi = M_PI * 2;
    double out = std::fmod(a, twoPi);
    if (out < 0)
        out += twoPi;
    return out;
}

namespace {
// 采样相邻标量关键帧；有效切线使用 Hermite 插值，否则回退到线性插值。
double evalScalarSegment(const ScalarStop &a, const ScalarStop &b, double t)
{
    const double dt = std::max(1e-6, b.t - a.t);
    const double s = clamp((t - a.t) / dt, 0, 1);

    const bool hasTangents = std::isfinite(a.outSlope) && std::isfinite(b.inSlope);
    if (!hasTangents)
        return lerp(a.v, b.v, s);

    // Unity AnimationCurve 非加权情形可近似为三次 Hermite。
    const double m0 = a.outSlope;
    const double m1 = b.inSlope;
    const double s2 = s * s;
    const double s3 = s2 * s;

    const double h00 = 2 * s3 - 3 * s2 + 1;
    const double h10 = s3 - 2 * s2 + s;
    const double h01 = -2 * s3 + 3 * s2;
    const double h11 = s3 - s2;

    return h00 * a.v + h10 * dt * m0 + h01 * b.v + h11 * dt * m1;
}

} // namespace

double evalScalarStops(const ScalarCurve &stops, double t, double fallback)
{
    if (stops.empty())
        return fallback;

    if (t <= stops[0].t)
        return stops[0].v;

    for (std::size_t i = 1; i < stops.size; i++) {
        if (t <= stops[i].t)
            return evalScalarSegment(stops[i - 1], stops[i], t);
    }

    return stops.back().v;
}

Rgb evalColorStops(const ColorCurve &stops, double t, const Rgb &fallback)
{
    if (stops.empty())
        return fallback;

    if (t <= stops[0].t)
        return stops[0].c;

    for (std::size_t i = 1; i < stops.size; i++) {
        if (t <= stops[i].t) {
            const ColorStop &a = stops[i - 1];
            const ColorStop &b = stops[i];
            const double lt = (t - a.t) / std::max(1e-6, b.t - a.t);
            return Rgb{
                lerp(a.c[0], b.c[0], lt),
                lerp(a.c[1], b.c[1], lt),
                lerp(a.c[2], b.c[2], lt),
            };
        }
    }

    return stops.back().c;
}

Rng::Rng(std::uint64_t seed)
    : m_engine(seed)
{
}

Rng::Rng()
    : m_engine(std::random_device{}())
{
}

double Rng::range(double a, double b)
{
    if (a == b)
        return a;
    return std::uniform_real_distribution<double>(a, b)(m_engine);
}

} // namespace baclickfx
