// SPDX-License-Identifier: GPL-3.0-or-later

#include "curveutils.h"
#include "damageutils.h"
#include "subsystems.h"
#include "trailstream.h"

#include <QTest>

#include <cmath>

using namespace KWin;

class LogicTests final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void scalarCurveClampsAndInterpolates();
    void hermiteCurveUsesTangents();
    void trailStreamExpiresOldPoints();
    void trailStreamBoundsIncludePadding();
    void distanceStepUsesWorldUnits();
    void particleCapLimitsBurst();
    void damageQuantizationGrowsOutward();
    void layerProgressClampsLifetime();
};

void LogicTests::scalarCurveClampsAndInterpolates()
{
    const std::array<baclickfx::ScalarStop, 2> stops{{{0.25, 2.0}, {0.75, 6.0}}};
    QCOMPARE(baclickfx::evalScalarStops(stops, 0.0), 2.0);
    QCOMPARE(baclickfx::evalScalarStops(stops, 1.0), 6.0);
    QCOMPARE(baclickfx::evalScalarStops(stops, 0.5), 4.0);
}

void LogicTests::hermiteCurveUsesTangents()
{
    const std::array<baclickfx::ScalarStop, 2> stops{{
        {0.0, 0.0, 0.0, 2.0},
        {1.0, 1.0, 2.0, 0.0},
    }};
    QVERIFY(std::abs(baclickfx::evalScalarStops(stops, 0.5) - 0.5) < 1e-9);
}

void LogicTests::trailStreamExpiresOldPoints()
{
    TrailStream stream;
    stream.beginStroke(QPointF(10, 20));
    stream.addSegment(QPointF(10, 20), QPointF(30, 40));
    QVERIFY(!stream.advance(0.25, 1.0));
    QVERIFY(stream.advance(0.8, 1.0));
    QVERIFY(stream.empty());
}

void LogicTests::trailStreamBoundsIncludePadding()
{
    TrailStream stream;
    stream.beginStroke(QPointF(10, 20));
    stream.addSegment(QPointF(10, 20), QPointF(30, 40));
    const QRectF bounds = stream.boundingBox(5.0);
    QCOMPARE(bounds, QRectF(5, 15, 30, 30));
}

void LogicTests::distanceStepUsesWorldUnits()
{
    baclickfx::Subsystem params;
    params.worldUnitPx = 540.0;
    params.rateOverDistance = 5.0;
    QCOMPARE(distanceStepFromRing4(params), 108.0);
    params.rateOverDistance = 0.0;
    QVERIFY(std::isinf(distanceStepFromRing4(params)));
}

void LogicTests::particleCapLimitsBurst()
{
    baclickfx::Subsystem params;
    params.distanceBurst = 4;
    params.maxParticles = 64;
    params.worldUnitPx = 540.0;
    baclickfx::Rng rng(1234);
    const TriBurstInstance burst = makeTriBurstInstance(
        QPointF(0, 0), params, 20, 1.0, 7, rng);
    QCOMPARE(static_cast<int>(burst.burst.particles.size()), 7);
}

void LogicTests::damageQuantizationGrowsOutward()
{
    const QRect result = baclickfx::quantizeDamageRect(QRectF(65.2, 63.1, 2.0, 2.0));
    QCOMPARE(result, QRect(64, 0, 64, 128));
    QVERIFY(baclickfx::quantizeDamageRect(QRectF()).isEmpty());
}

void LogicTests::layerProgressClampsLifetime()
{
    QCOMPARE(layerProgress(2.0, -1.0), 0.0);
    QCOMPARE(layerProgress(2.0, 1.0), 0.5);
    QCOMPARE(layerProgress(2.0, 3.0), 1.0);
    QCOMPARE(layerProgress(0.0, 0.0), 1.0);
}

QTEST_APPLESS_MAIN(LogicTests)
#include "logic_tests.moc"
