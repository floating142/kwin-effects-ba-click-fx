// SPDX-License-Identifier: GPL-3.0-or-later

#include "applicationfilter.h"
#include "curveutils.h"
#include "clickinstance.h"
#include "bloomutils.h"
#include "damageutils.h"
#include "diagnostics.h"
#include "meshprofiles.h"
#include "pathresampler.h"
#include "outputscaleutils.h"
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
    void outputScaleUsesUnityOrthographicProjection();
    void outputScaleIdIsStable();
    void outputUuidIsPreferred();
    void applicationFilterNormalizesAndRoundTrips();
    void applicationIdentityUsesPriority();
    void processIdentityIgnoresEphemeralLutrisId();
    void processIdentityReadsCurrentProcess();
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

void LogicTests::outputScaleUsesUnityOrthographicProjection()
{
    const auto base = baclickfx::buildSubsystemMap(1.0, 1.0, 1080.0);
    const auto large = baclickfx::buildSubsystemMap(1.0, 1.4, 1080.0);
    QVERIFY(std::abs(base.ring.worldUnitPx - (1080.0 / (2.0 * baclickfx::kUnityOrthographicSize))) < 1e-9);
    QVERIFY(std::abs(large.ring.worldUnitPx - base.ring.worldUnitPx * 1.4) < 1e-9);
}

void LogicTests::outputScaleIdIsStable()
{
    QCOMPARE(baclickfx::outputScaleId(QStringLiteral("Vendor"), QStringLiteral("Model"),
                                      QStringLiteral("Serial"), QStringLiteral("DP-1")),
             QStringLiteral("Vendor|Model|Serial|DP-1"));
}

void LogicTests::outputUuidIsPreferred()
{
    QCOMPARE(baclickfx::preferredOutputScaleId(QStringLiteral("uuid-1"), QStringLiteral("V"),
                                                QStringLiteral("M"), QStringLiteral("S"),
                                                QStringLiteral("DP-1")),
             QStringLiteral("uuid-1"));
    QCOMPARE(baclickfx::preferredOutputScaleId({}, QStringLiteral("V"), QStringLiteral("M"),
                                                QStringLiteral("S"), QStringLiteral("DP-1")),
             QStringLiteral("V|M|S|DP-1"));
}

void LogicTests::applicationFilterNormalizesAndRoundTrips()
{
    const QVector<baclickfx::ExcludedApplication> source{{
        .identity = {
            baclickfx::ApplicationIdentityKind::DesktopFile,
            QStringLiteral(" Org.KDE.Kate.desktop "),
            QStringLiteral("ignored"),
        },
        .displayName = QStringLiteral("Kate"),
    }};
    const QByteArray json = baclickfx::serializeExcludedApplications(source);
    QVERIFY(json.contains("\"kind\":\"desktop-file\""));
    QVERIFY(!json.contains("desktopFile"));
    const auto parsed = baclickfx::parseExcludedApplications(json);
    QCOMPARE(parsed.size(), 1);
    QCOMPARE(parsed.front().identity.kind,
             baclickfx::ApplicationIdentityKind::DesktopFile);
    QCOMPARE(parsed.front().identity.value, QStringLiteral("org.kde.kate"));
    QVERIFY(parsed.front().identity.qualifier.isEmpty());
    QCOMPARE(parsed.front().displayName, QStringLiteral("Kate"));

    // 旧的多字段对象没有 kind/value，必须直接忽略而不是隐式迁移。
    const QByteArray oldFormat = R"([{"desktopFile":"org.kde.kate","resourceClass":"kate"}])";
    QVERIFY(baclickfx::parseExcludedApplications(oldFormat).isEmpty());
    const QByteArray broadLauncher =
        R"([{"kind":"launcher","value":"lutris:random-run-id","qualifier":"game.exe"}])";
    QVERIFY(baclickfx::parseExcludedApplications(broadLauncher).isEmpty());
}

void LogicTests::applicationIdentityUsesPriority()
{
    const baclickfx::ProcessIdentity steamProcess{
        .launcherId = QStringLiteral("steam:1672970"),
        .command = QStringLiteral("c:/games/game-one.exe"),
        .winePrefix = QStringLiteral("/games/prefix-one"),
    };
    const auto desktop = baclickfx::identifyApplication(
        QStringLiteral("Org.KDE.Kate.desktop"), QStringLiteral("other"), {}, steamProcess);
    QCOMPARE(desktop.kind, baclickfx::ApplicationIdentityKind::DesktopFile);
    QCOMPARE(desktop.value, QStringLiteral("org.kde.kate"));

    const auto launcher = baclickfx::identifyApplication(
        {}, QStringLiteral("steam_app_default"), QStringLiteral("steam_app_default"),
        steamProcess);
    QCOMPARE(launcher.kind, baclickfx::ApplicationIdentityKind::Launcher);
    QCOMPARE(launcher.value, QStringLiteral("steam:1672970"));
    QCOMPARE(launcher.qualifier, QStringLiteral("game-one.exe"));

    baclickfx::ProcessIdentity launcherProcess = steamProcess;
    launcherProcess.command = QStringLiteral("c:/launcher/games.exe");
    const auto gameLauncher = baclickfx::identifyApplication(
        {}, QStringLiteral("steam_app_default"), QStringLiteral("steam_app_default"),
        launcherProcess);
    QCOMPARE(gameLauncher.value, launcher.value);
    QCOMPARE(gameLauncher.qualifier, QStringLiteral("games.exe"));

    baclickfx::ProcessIdentity plainProcess = steamProcess;
    plainProcess.launcherId.clear();
    const auto process = baclickfx::identifyApplication(
        {}, QStringLiteral("steam_app_default"), QStringLiteral("steam_app_default"),
        plainProcess);
    QCOMPARE(process.kind, baclickfx::ApplicationIdentityKind::Process);
    QCOMPARE(process.value, QStringLiteral("game-one.exe"));
    QCOMPARE(process.qualifier, QStringLiteral("/games/prefix-one"));

    const auto windowClass = baclickfx::identifyApplication(
        {}, QStringLiteral("Wine-Game"), QStringLiteral("Game-One"));
    QCOMPARE(windowClass.kind, baclickfx::ApplicationIdentityKind::WindowClass);
    QCOMPARE(windowClass.value, QStringLiteral("wine-game"));
    QCOMPARE(windowClass.qualifier, QStringLiteral("game-one"));

    const QVector<baclickfx::ExcludedApplication> rules{{launcher, QStringLiteral("Game")}};
    QVERIFY(baclickfx::isApplicationExcluded(rules, launcher));
    QVERIFY(!baclickfx::isApplicationExcluded(rules, gameLauncher));
    QVERIFY(!baclickfx::isApplicationExcluded(rules, process));
}

void LogicTests::processIdentityIgnoresEphemeralLutrisId()
{
    QByteArray commandLine("C:\\Games\\Game.exe");
    commandLine.append('\0');
    commandLine.append("--fullscreen");

    QByteArray lutrisEnvironment("SteamAppId=default");
    lutrisEnvironment.append('\0');
    lutrisEnvironment.append("SteamGameId=default");
    lutrisEnvironment.append('\0');
    lutrisEnvironment.append("LUTRIS_GAME_UUID=253781D3-8B1A-4C75-BD22-7C5EF9DED22B");
    lutrisEnvironment.append('\0');
    lutrisEnvironment.append("WINEPREFIX=/games/prefix/");
    const auto lutris = baclickfx::processIdentityFromData(commandLine, lutrisEnvironment);
    QVERIFY(lutris.launcherId.isEmpty());
    QCOMPARE(lutris.command, QStringLiteral("c:/games/game.exe"));
    QCOMPARE(lutris.winePrefix, QStringLiteral("/games/prefix"));
    const auto lutrisIdentity = baclickfx::identifyApplication(
        {}, QStringLiteral("steam_app_default"), QStringLiteral("steam_app_default"), lutris);
    QCOMPARE(lutrisIdentity.kind, baclickfx::ApplicationIdentityKind::Process);
    QCOMPARE(lutrisIdentity.value, QStringLiteral("game.exe"));
    QCOMPARE(lutrisIdentity.qualifier, QStringLiteral("/games/prefix"));

    QByteArray steamEnvironment("SteamAppId=1672970");
    steamEnvironment.append('\0');
    steamEnvironment.append("SteamGameId=1672970");
    const auto steam = baclickfx::processIdentityFromData(commandLine, steamEnvironment);
    QCOMPARE(steam.launcherId, QStringLiteral("steam:1672970"));

    QByteArray fallbackEnvironment("SteamAppId=default");
    fallbackEnvironment.append('\0');
    fallbackEnvironment.append("SteamGameId=default");
    fallbackEnvironment.append('\0');
    fallbackEnvironment.append("WINEPREFIX=/games/fallback");
    const auto fallback = baclickfx::processIdentityFromData(
        commandLine, fallbackEnvironment);
    QVERIFY(fallback.launcherId.isEmpty());
    QCOMPARE(fallback.winePrefix, QStringLiteral("/games/fallback"));
}

void LogicTests::processIdentityReadsCurrentProcess()
{
    const baclickfx::ProcessIdentity identity = baclickfx::processIdentity(
        QCoreApplication::applicationPid());
    QVERIFY(!identity.command.isEmpty());
}

QTEST_APPLESS_MAIN(LogicTests)
#include "logic_tests.moc"
