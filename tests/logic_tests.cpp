// SPDX-License-Identifier: GPL-3.0-or-later

#include "curveutils.h"
#include "clickinstance.h"
#include "bloomutils.h"
#include "damageutils.h"
#include "diagnostics.h"
#include "meshprofiles.h"
#include "pathresampler.h"
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
    void trailStreamKeepsNewestPointsAtCapacity();
    void distanceStepUsesWorldUnits();
    void particleCapLimitsBurst();
    void damageQuantizationGrowsOutward();
    void layerProgressClampsLifetime();
    void pathResamplingIsEventSegmentationInvariant();
    void meshProfileLoadsObjAndUv();
    void frameStatsHaveStableFields();
    void uvFrameTileUsesDiscreteAtlasFrames();
    void subColorClampsProgress();
    void trailStrokesPreservePathEndpoints();
    void meshTriRotationIntegratesCurveAndClampsMix();
    void bloomPyramidMatchesPpv2Formula();
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

void LogicTests::trailStreamKeepsNewestPointsAtCapacity()
{
    TrailStream stream;
    stream.beginStroke(QPointF(0, 0));
    for (int i = 1; i <= 1100; ++i) {
        stream.addSegment(QPointF(i - 1, 0), QPointF(i, 0));
    }

    const auto &points = stream.points();
    QCOMPARE(points.size(), std::size_t(1000));
    QCOMPARE(points.front().pos, QPointF(101, 0));
    QCOMPARE(points.back().pos, QPointF(1100, 0));
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

void LogicTests::pathResamplingIsEventSegmentationInvariant()
{
    const auto one = baclickfx::resamplePathSegment(QPointF(0, 0), QPointF(25, 0), 5.0, 0.0);
    const auto first = baclickfx::resamplePathSegment(QPointF(0, 0), QPointF(12, 0), 5.0, 0.0);
    const auto second = baclickfx::resamplePathSegment(QPointF(12, 0), QPointF(25, 0), 5.0,
                                                       first.remainder);
    QCOMPARE(one.points.size(), std::size_t(5));
    QCOMPARE(first.points.size() + second.points.size(), one.points.size());
    for (std::size_t i = 0; i < one.points.size(); ++i) {
        const QPointF point = i < first.points.size() ? first.points[i] : second.points[i - first.points.size()];
        QCOMPARE(point, one.points[i]);
    }
}

void LogicTests::meshProfileLoadsObjAndUv()
{
    const std::string path = std::string(BA_CLICK_FX_SOURCE_DIR)
        + "/assets/Cylinder002.obj";
    const auto profile = baclickfx::loadCylinderObjProfile(path);
    QVERIFY(profile.has_value());
    QVERIFY(profile->segmentCount >= 16);
    QVERIFY(profile->mesh.valid());
    QVERIFY(!profile->mesh.uvs.empty());
    QVERIFY(!profile->mesh.uvFaces.empty());
    QVERIFY(profile->mesh.faceUv(0, 0).has_value());
    QVERIFY(profile->uvMax > profile->uvMin);
}

void LogicTests::frameStatsHaveStableFields()
{
    baclickfx::FrameStatsSummary stats;
    stats.frames = 3;
    stats.cpuAvgMs = 0.25;
    stats.output = QStringLiteral("1920x1080@1.00");
    const QString text = baclickfx::formatFrameStats(stats);
    QVERIFY(text.startsWith(QStringLiteral("event=frame_stats")));
    QVERIFY(text.contains(QStringLiteral("frames=3")));
    QVERIFY(text.contains(QStringLiteral("cpu_avg_ms=0.250")));
    QVERIFY(text.contains(QStringLiteral("output=\"1920x1080@1.00\"")));
    QVERIFY(text.contains(QStringLiteral("skip_import=0")));
}

void LogicTests::uvFrameTileUsesDiscreteAtlasFrames()
{
    baclickfx::Subsystem params;
    params.uvEnabled = true;
    params.uvTilesX = 2;
    params.uvTilesY = 1;
    const auto tile = baclickfx::uvFrameTile(params, 0.0, 0.0);
    QCOMPARE(tile.scaleX, 0.5);
    QCOMPARE(tile.scaleY, 1.0);
    QCOMPARE(tile.offsetX, 0.0);
    QCOMPARE(tile.offsetY, 0.0);
    QCOMPARE(tile.mapU(1.0), 0.5);
}

void LogicTests::subColorClampsProgress()
{
    const auto map = baclickfx::buildSubsystemMap(1.0, 1.0, 1080.0);
    const auto first = baclickfx::subColor(map.ring3, -1.0);
    const auto last = baclickfx::subColor(map.ring3, 2.0);
    QVERIFY(first.a >= 0.0 && first.a <= 1.0);
    QVERIFY(last.a >= 0.0 && last.a <= 1.0);
    QVERIFY(first.r >= 0.0 && first.g >= 0.0 && first.b >= 0.0);
    QVERIFY(last.r >= 0.0 && last.g >= 0.0 && last.b >= 0.0);
}

void LogicTests::trailStrokesPreservePathEndpoints()
{
    TrailStream stream;
    stream.beginStroke(QPointF(10, 20));
    stream.addSegment(QPointF(10, 20), QPointF(30, 20));
    const auto map = baclickfx::buildSubsystemMap(1.0, 1.0, 1080.0);
    const auto strokes = buildTrailStrokes(stream, map.trail);
    QCOMPARE(strokes.size(), std::size_t(1));
    QCOMPARE(strokes.front().samples.size(), std::size_t(2));
    QCOMPARE(strokes.front().samples.front().pos, QPointF(10, 20));
    QCOMPARE(strokes.front().samples.back().pos, QPointF(30, 20));
    QVERIFY(strokes.front().totalLength > 0.0);
}

void LogicTests::meshTriRotationIntegratesCurveAndClampsMix()
{
    static const baclickfx::ScalarStop minCurve[] = {{0.0, 1.0}, {1.0, 1.0}};
    static const baclickfx::ScalarStop maxCurve[] = {{0.0, 3.0}, {1.0, 3.0}};
    MeshTriEmission emission;
    emission.lifetimeSec = 2.0;
    emission.params.rotationEnabled = true;
    emission.params.rotationScalar = 1.0;
    emission.params.rotationCurveMin = minCurve;
    emission.params.rotationCurveMax = maxCurve;

    QCOMPARE(meshTriRotation(emission, 0.0, 0.5), 0.0);
    QCOMPARE(meshTriRotation(emission, 0.5, -1.0), -1.0);
    QCOMPARE(meshTriRotation(emission, 0.5, 0.5), -2.0);
    QCOMPARE(meshTriRotation(emission, 0.5, 2.0), -3.0);
}

void LogicTests::bloomPyramidMatchesPpv2Formula()
{
    QCOMPARE(baclickfx::bloomBaseSize(QSize(3200, 2000)), QSize(1600, 1000));
    const auto params = baclickfx::bloomPyramidParams(QSize(1600, 1000));
    QCOMPARE(params.iterations, 7);
    QVERIFY(std::abs(double(params.sampleScale) - 1.143856) < 1e-5);
    QVERIFY(std::abs(baclickfx::bloomReachPx(QSize(3200, 2000), 1.6)
                     - 171.50848) < 1e-4);
}

QTEST_APPLESS_MAIN(LogicTests)
#include "logic_tests.moc"
