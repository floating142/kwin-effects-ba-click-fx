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
#include "mousebuttonutils.h"
#include "baclickfxdefaults.h"
#include "subsystems.h"
#include "trailstream.h"
#include "unity/rotation.h"
#include "unity/timing.h"
#include "frameclock.h"

#include <QTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QImage>

#include <cmath>
#include <bit>
#include <span>

using namespace KWin;

class LogicTests final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void scalarCurveClampsAndInterpolates();
    void hermiteCurveUsesTangents();
    void trailStreamExpiresOldPoints();
    void trailTailClipsContinuouslyAndHeadStaysLive();
    void trailPointsMatchNativeSnapshots();
    void trailStreamBoundsIncludePadding();
    void trailStreamKeepsNewestPointsAtCapacity();
    void distanceStepUsesWorldUnits();
    void particleCapLimitsBurst();
    void damageQuantizationGrowsOutward();
    void pointerPixelRespectsOutputSeams();
    void particleProgressMatchesPercentAge();
    void pathResamplingIsEventSegmentationInvariant();
    void meshProfileLoadsObjAndUv();
    void frameStatsHaveStableFields();
    void uvFrameTileUsesDiscreteAtlasFrames();
    void subColorClampsProgress();
    void trailStrokesPreservePathEndpoints();
    void meshTriWorldRotationStepsBeforeAgeAdvance();
    void bloomPyramidMatchesPpv2Formula();
    void outputScaleUsesUnityOrthographicProjection();
    void outputScaleIdIsStable();
    void outputUuidIsPreferred();
    void applicationFilterNormalizesAndRoundTrips();
    void applicationIdentityUsesPriority();
    void processIdentityIgnoresEphemeralLutrisId();
    void processIdentityReadsCurrentProcess();
    void buttonTriggersDefaultToLeftOnly();
    void buttonChordKeepsOneGestureUntilLastRelease();
    void buttonTriggersIgnoreUnsupportedAndEmptySelections();
    void particleColorsMatchNativeSnapshots();
    void triangleBirthMatchesNativeDispatcher();
    void birthFrameTimingAndCapacityMatchNative();
    void trailMeshAndPackedColorsMatchNative();
    void billboardLimitsApplyToNarrowOutputsAndDamageSize();
    void unityParametersRetainSerializedPrecisionAndCounts();
    void texturesMatchOriginalPlayerResources();
    void rotationSpeedsMatchNativeSnapshots();
    void meshMatchesSourceUnityCoordinates();
    void motionMatchesNativeSnapshots();
    void nativeFrameSplitsAtSourceMaximumStep();
    void nativeRetirementKeepsExactLifetimeBoundary();
    void frameSamplingFollowsScreenRefreshRate();
    void coldClickAdvancesOnSecondFrame();
    void longFrameUsesSeparateParticleAndTrailClocks();
    void mixedRefreshSamplingUsesCursorOutput();
    void frameBirthsShareOnePaddedNativeBatch();
};

void LogicTests::frameSamplingFollowsScreenRefreshRate()
{
    for (int rate : {60, 144, 240}) {
        baclickfx::OutputFrameClock clock;
        const auto period = std::chrono::nanoseconds(1000000000 / rate);
        int accepted = 0;
        std::chrono::nanoseconds elapsed{};
        for (int i = 0; i < rate; ++i) {
            const auto tick = clock.take(1, 1, period * i);
            accepted += tick.accepted;
            elapsed += tick.elapsed;
            QVERIFY(!clock.take(1, 1, period * i).accepted); // 同一帧不能因多次绘制重复发射。
        }
        QCOMPARE(accepted, rate);
        QCOMPARE(elapsed, period * (rate - 1));
    }
}

void LogicTests::coldClickAdvancesOnSecondFrame()
{
    const auto params = baclickfx::buildSubsystemMap(1, 1);
    for (int rate : {60, 144, 240}) {
        baclickfx::OutputFrameClock clock;
        baclickfx::Rng rng(42);
        auto click = makeClickInstance(QPointF(100, 100), params, 1, rng);
        const auto period = std::chrono::nanoseconds(1000000000 / rate);
        const auto first = clock.take(1, 1, period);
        QVERIFY(first.accepted);
        QCOMPARE(first.elapsed.count(), 0);
        advanceClickFrame(click, std::chrono::duration<double>(first.elapsed).count());
        QVERIFY(!click.birthPending);
        QCOMPARE(click.ring.percentAge, 0.f);
        const auto second = clock.take(1, 1, period * 2);
        const double elapsed = std::chrono::duration<double>(second.elapsed).count();
        advanceClickFrame(click, elapsed);
        QCOMPARE(click.ring.percentAge,
            baclickfx::unity::advancePercentAge(0, float(elapsed), click.ring.inverseLifetime));
        QVERIFY(!click.ring3.particles.empty());
        const auto &triangle = click.ring3.particles.front();
        QCOMPARE(triangle.percentAge,
            baclickfx::unity::advancePercentAge(0, float(elapsed), triangle.inverseLifetime));
    }
}

void LogicTests::longFrameUsesSeparateParticleAndTrailClocks()
{
    using namespace std::chrono_literals;
    baclickfx::OutputFrameClock clock;
    baclickfx::Rng rng(42);
    const auto params = baclickfx::buildSubsystemMap(1, 1);
    auto click = makeClickInstance(QPointF(0, 0), params, 1, rng);
    QVERIFY(clock.take(1, 1, 16ms).accepted);
    advanceClickFrame(click, 0);
    const auto longFrame = clock.take(1, 1, 816ms);
    QCOMPARE(longFrame.elapsed, 800ms);
    const double elapsed = std::chrono::duration<double>(longFrame.elapsed).count();
    advanceClickFrame(click, elapsed);
    QVERIFY(click.ring3.particles.empty());

    TrailStream stream;
    stream.beginStroke(QPointF(0, 0));
    stream.advanceTime(std::min(elapsed, double(baclickfx::unity::sourceMaximumAllowedTimestep)));
    QVERIFY(!stream.retireExpired(0.3));
    QCOMPARE(stream.time(), 0.1f);

    // A new point keeps the expired predecessor for clipping on this same frame.
    stream.advanceTime(0.25);
    stream.addSegment(QPointF(0, 0), QPointF(10, 0));
    QVERIFY(!stream.retireExpired(0.3));
    QCOMPARE(stream.points().size(), std::size_t(2));
    QCOMPARE(stream.points().back().birthTime, stream.time());
}

void LogicTests::mixedRefreshSamplingUsesCursorOutput()
{
    using namespace std::chrono_literals;
    baclickfx::OutputFrameClock clock;
    QVERIFY(clock.take(2, 2, 7ms).accepted);
    QVERIFY(!clock.take(1, 2, 16ms).accepted);
    QCOMPARE(clock.take(2, 2, 14ms).elapsed, 7ms);
    // 切换到另一输出时，即使它的预测时间早于已模拟时刻，也不能倒退或重复累加。
    const auto switched = clock.take(1, 1, 11ms);
    QVERIFY(!switched.accepted);
    QCOMPARE(switched.elapsed, 0ns);
    QVERIFY(!clock.take(2, 1, 21ms).accepted);
    QCOMPARE(clock.take(1, 1, 27ms).elapsed, 13ms);
    QVERIFY(!clock.take(2, 2, 27ms).accepted); // 同时刻换屏也不能重新采样。

    // Emission must retain the unsampled crossing distance until a newer frame.
    baclickfx::OutputFrameClock crossing;
    baclickfx::unity::EmissionState emission;
    emission.init(42);
    float previous = 0;
    const auto sample = [&](std::uintptr_t output, auto now, float position) {
        const auto tick = crossing.take(output, output, now);
        if (!tick.accepted) return std::uint64_t(0);
        const float dt = std::chrono::duration<float>(tick.elapsed).count();
        const auto count = dt > 0 ? baclickfx::unity::advanceDistanceEmission(emission, 5,
            {((position - previous) / 540.f) / dt, 0, 0}, dt) : 0;
        previous = position;
        return std::uint64_t(count);
    };
    QCOMPARE(sample(2, 14ms, 0), std::uint64_t(0));
    QCOMPARE(sample(1, 11ms, 54), std::uint64_t(0));
    QCOMPARE(previous, 0.f);
    QCOMPARE(sample(1, 27ms, 120), std::uint64_t(1));
    clock.reset();
    QCOMPARE(clock.take(2, 2, 100ms).elapsed, 0ns);
}

void LogicTests::frameBirthsShareOnePaddedNativeBatch()
{
    const auto params = baclickfx::buildSubsystemMap(1, 1).ring4;
    ParticleEmitterState wholeState(42), splitState(42);
    baclickfx::Rng entropy(0);
    wholeState.beginEmission(7);
    splitState.beginEmission(7);
    const auto whole = emitTriBurst(params, 1, 7, entropy, &wholeState);
    for (std::size_t i = 0; i < 7; ++i) {
        const auto split = emitTriBurst(params, 1, 1, entropy, &splitState);
        const auto &actual = split.particles.front();
        const auto &expected = whole.particles[i];
        QCOMPARE(actual.seed, expected.seed);
        QCOMPARE(actual.worldMotion.position, expected.worldMotion.position);
        QCOMPARE(actual.worldMotion.velocity, expected.worldMotion.velocity);
        QCOMPARE(actual.inverseLifetime, expected.inverseLifetime);
        QCOMPARE(actual.startSize, expected.startSize);
        QCOMPARE(actual.uvStartFrameNorm, expected.uvStartFrameNorm);
    }
    QCOMPARE(wholeState.initial.state(), splitState.initial.state());
    QCOMPARE(wholeState.shape.state(), splitState.shape.state());
}

void LogicTests::motionMatchesNativeSnapshots()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-motion-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto samples = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("samples")].toArray();
    QVERIFY(samples.size() >= 100);
    for (const auto &entry : samples) {
        const auto row = entry.toObject();
        const auto before = row[QStringLiteral("before")].toArray();
        const auto after = row[QStringLiteral("after")].toArray();
        const auto module = row[QStringLiteral("moduleVelocity")].toArray();
        baclickfx::unity::Motion motion;
        baclickfx::unity::MotionVector velocity;
        for (int axis = 0; axis < 3; ++axis) {
            motion.position[std::size_t(axis)] = float(before[axis].toDouble());
            motion.velocity[std::size_t(axis)] = float(before[axis + 3].toDouble());
            velocity[std::size_t(axis)] = float(module[axis].toDouble());
        }
        const float dt = float(row[QStringLiteral("dt")].toDouble());
        baclickfx::unity::integrateMotion(motion, velocity, dt);
        const float percent = baclickfx::unity::advancePercentAge(float(before[6].toDouble()), dt,
            float(row[QStringLiteral("inverseLifetime")].toDouble()));
        for (int field = 0; field < 7; ++field) {
            const float actual = field < 3 ? motion.position[std::size_t(field)]
                : field < 6 ? motion.velocity[std::size_t(field - 3)] : percent;
            QCOMPARE(std::bit_cast<std::uint32_t>(actual),
                std::bit_cast<std::uint32_t>(float(after[field].toDouble())));
        }
    }
}

void LogicTests::nativeFrameSplitsAtSourceMaximumStep()
{
    const auto map = baclickfx::buildSubsystemMap(1, 1);
    baclickfx::Rng seed(42);
    auto frame = makeClickInstance({}, map, 1, seed);
    auto split = frame;
    const float tick = 1.f / 60;
    advanceClickInstance(frame, double(1.f / 30));
    advanceClickInstance(split, double(tick));
    advanceClickInstance(split, double(tick));
    QCOMPARE(frame.ring.percentAge, split.ring.percentAge);
    QCOMPARE(frame.meshTri.percentAge, split.meshTri.percentAge);
    QCOMPARE(frame.ring3.particles.size(), split.ring3.particles.size());
    for (std::size_t i = 0; i < frame.ring3.particles.size(); ++i) {
        QCOMPARE(frame.ring3.particles[i].worldMotion.position, split.ring3.particles[i].worldMotion.position);
        QCOMPARE(frame.ring3.particles[i].percentAge, split.ring3.particles[i].percentAge);
    }
    for (std::size_t i = 0; i < frame.meshTri.arcs.size(); ++i)
        QCOMPARE(frame.meshTri.arcs[i].angle, split.meshTri.arcs[i].angle);
}

void LogicTests::nativeRetirementKeepsExactLifetimeBoundary()
{
    TriBurstEmission burst;
    burst.particles.resize(5);
    const float ages[]{100.f, 100.00000762939453f, 99.f, 100.00000762939453f, 98.f};
    for (std::size_t i = 0; i < 5; ++i) {
        burst.particles[i].seed = std::uint32_t(i);
        burst.particles[i].percentAge = ages[i];
        burst.particles[i].inverseLifetime = 1;
    }
    advanceTriBurstFrame(burst, .000001);
    // 原生逐 lane 从后往前退役，以最后一个存活者覆盖；等于 100 的粒子在推进后退役。
    QCOMPARE(burst.particles.size(), std::size_t(2));
    QCOMPARE(burst.particles[0].seed, std::uint32_t(2));
    QCOMPARE(burst.particles[1].seed, std::uint32_t(4));
    QVERIFY(baclickfx::unity::survives(100.f));
    QVERIFY(!baclickfx::unity::survives(100.00000762939453f));
}

void LogicTests::meshMatchesSourceUnityCoordinates()
{
    QFile source(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-touch.json"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(source.readAll()).object()[QStringLiteral("meshes")]
        .toObject()[QStringLiteral("Cylinder002")].toObject();
    const auto mesh = baclickfx::loadCylinderObjProfile(BA_CLICK_FX_SOURCE_DIR "/assets/Cylinder002.obj");
    QVERIFY(mesh.has_value());
    const auto vertices = original[QStringLiteral("vertices")].toArray();
    const auto faces = original[QStringLiteral("triangles")].toArray();
    const auto uvs = original[QStringLiteral("uv")].toArray();
    QCOMPARE(mesh->mesh.vertices.size(), std::size_t(vertices.size()));
    QCOMPARE(mesh->mesh.faces.size(), std::size_t(faces.size()));
    for (int i = 0; i < vertices.size(); ++i) {
        const auto point = vertices[i].toArray();
        const auto screen = baclickfx::unityObjPointOffset(mesh->mesh.vertices[std::size_t(i)], 1, 1, 0);
        QVERIFY(std::abs(screen[0] - point[0].toDouble()) < 1e-7);
        QVERIFY(std::abs(screen[1] + point[1].toDouble()) < 1e-7);
    }
    for (int i = 0; i < faces.size(); ++i) {
        const auto face = faces[i].toArray();
        for (int corner = 0; corner < 3; ++corner) {
            const auto index = mesh->mesh.faces[std::size_t(i)][std::size_t(corner)];
            QCOMPARE(index, face[2 - corner].toInt());
            const auto actualUv = mesh->mesh.faceUv(std::size_t(i), corner);
            QVERIFY(actualUv.has_value());
            const auto uv = uvs[index].toArray();
            QVERIFY(std::abs((*actualUv)[0] - uv[0].toDouble()) < 1e-7);
            QVERIFY(std::abs((*actualUv)[1] - uv[1].toDouble()) < 1e-7);
        }
    }
}

void LogicTests::rotationSpeedsMatchNativeSnapshots()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-rotation-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto samples = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("samples")].toArray();
    QVERIFY(samples.size() >= 128);
    const auto params = baclickfx::buildSubsystemMap(1, 1).meshTri;
    for (const auto &entry : samples) {
        const auto sample = entry.toObject();
        const float actual = baclickfx::unity::rotationSpeed(
            std::span(params.rotationCurveMin.data, params.rotationCurveMin.size),
            std::span(params.rotationCurveMax.data, params.rotationCurveMax.size), float(params.rotationScalar),
            float(sample[QStringLiteral("progress")].toDouble()),
            std::uint32_t(sample[QStringLiteral("seed")].toDouble()));
        const float expected = float(sample[QStringLiteral("speed")].toDouble());
        QCOMPARE(std::bit_cast<std::uint32_t>(actual), std::bit_cast<std::uint32_t>(expected));
    }
}

void LogicTests::texturesMatchOriginalPlayerResources()
{
    QFile source(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-touch.json"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const auto textures = QJsonDocument::fromJson(source.readAll()).object()[QStringLiteral("textures")].toObject();
    QCOMPARE(textures.size(), 4);
    for (auto it = textures.begin(); it != textures.end(); ++it) {
        const auto expected = it.value().toObject();
        const QImage image = QImage(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/assets/") + it.key())
            .convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(!image.isNull());
        QCOMPARE(image.width(), expected[QStringLiteral("width")].toInt());
        QCOMPARE(image.height(), expected[QStringLiteral("height")].toInt());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (int row = 0; row < image.height(); ++row) {
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(row)), image.width() * 4));
        }
        QCOMPARE(QString::fromLatin1(hash.result().toHex()), expected[QStringLiteral("rgbaSha256")].toString());
    }
}

void LogicTests::particleColorsMatchNativeSnapshots()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-gradient-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    const auto map = baclickfx::buildSubsystemMap(1, 1);
    const QMap<QString, const baclickfx::Subsystem *> systems{
        {QStringLiteral("Ring"), &map.ring}, {QStringLiteral("Ring3"), &map.ring3}, {QStringLiteral("Ring4"), &map.ring4}, {QStringLiteral("MeshTri"), &map.meshTri}};
    const auto samples = data[QStringLiteral("samples")].toArray();
    QVERIFY(samples.size() > 300);
    for (const auto &entry : samples) {
        const auto sample = entry.toObject();
        const auto *system = systems.value(sample[QStringLiteral("system")].toString());
        QVERIFY(system);
        const auto actual = baclickfx::subColor(*system, sample[QStringLiteral("age")].toDouble());
        const auto rgba = sample[QStringLiteral("rgba")].toArray();
        const double channels[]{actual.r, actual.g, actual.b, actual.a};
        for (int ch = 0; ch < 4; ++ch) {
            const double encoded = rgba[ch].toInt() / 255.0;
            const double expected = ch < 3 ? baclickfx::srgbToLinearChannel(encoded) : encoded;
            QVERIFY2(std::abs(channels[ch] - expected) < 1e-12,
                qPrintable(QStringLiteral("%1 age=%2 channel=%3 expected=%4 actual=%5")
                    .arg(sample[QStringLiteral("system")].toString()).arg(sample[QStringLiteral("age")].toDouble(), 0, 'g', 12)
                    .arg(ch).arg(expected).arg(channels[ch])));
        }
    }
}

void LogicTests::triangleBirthMatchesNativeDispatcher()
{
    const auto map = baclickfx::buildSubsystemMap(1, 1, 1080);
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-birth-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto samples = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("samples")].toArray();
    QVERIFY(samples.size() >= 200);
    for (const auto &entry : samples) {
        const auto row = entry.toObject();
        const auto &params = row[QStringLiteral("system")].toInt() == 3 ? map.ring3 : map.ring4;
        ParticleEmitterState state(std::uint32_t(row[QStringLiteral("seed")].toDouble()));
        const int count = row[QStringLiteral("count")].toInt();
        state.beginEmission(std::uint64_t(count));
        baclickfx::Rng entropy(0);
        const auto burst = emitTriBurst(params, 1, count, entropy, &state);
        const auto &particle = burst.particles[std::size_t(row[QStringLiteral("lane")].toInt())];
        const auto position = row[QStringLiteral("position")].toArray();
        const auto velocity = row[QStringLiteral("velocity")].toArray();
        for (int axis = 0; axis < 3; ++axis) {
            QCOMPARE(std::bit_cast<std::uint32_t>(particle.worldMotion.position[std::size_t(axis)]),
                std::bit_cast<std::uint32_t>(float(position[axis].toDouble())));
            QCOMPARE(std::bit_cast<std::uint32_t>(particle.worldMotion.velocity[std::size_t(axis)]),
                std::bit_cast<std::uint32_t>(float(velocity[axis].toDouble())));
        }
        // Source world velocities already include the emitter scale once.
        auto moved = particle;
        baclickfx::unity::integrateMotion(moved.worldMotion, {}, .1f);
        const QPointF delta = triParticleOffset(moved, params) - triParticleOffset(particle, params);
        QVERIFY(std::abs(delta.x() - particle.worldMotion.velocity[0] * .1 * 540) < 1e-5);
        QVERIFY(std::abs(delta.y() + particle.worldMotion.velocity[1] * .1 * 540) < 1e-5);
    }
}

void LogicTests::birthFrameTimingAndCapacityMatchNative()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-birth-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto frames = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("frames")].toArray();
    QVERIFY(frames.size() >= 12);
    const auto map = baclickfx::buildSubsystemMap(1, 1, 1080);
    for (const auto &entry : frames) {
        const auto row = entry.toObject();
        const auto &params = row[QStringLiteral("system")].toInt() == 3 ? map.ring3 : map.ring4;
        const int previous = row[QStringLiteral("previous")].toInt(), count = row[QStringLiteral("count")].toInt();
        ParticleBirthFrame frame;
        frame.step = float(row[QStringLiteral("step")].toDouble());
        frame.offset = frame.step;
        frame.rateCount = std::uint64_t(count);
        frame.emission.carry = float(row[QStringLiteral("carry")].toDouble());
        frame.emission.intervalScale = float(row[QStringLiteral("interval")].toDouble());
        frame.emitterVelocity = {2,-1,.25f};
        ParticleEmitterState state(std::uint32_t(row[QStringLiteral("seed")].toDouble()));
        baclickfx::Rng rng(0);
        TriBurstEmission burst;
        burst.particles.resize(std::size_t(previous));
        appendTriBurst(burst, params, 1, std::min(count,params.maxParticles-previous), rng, state, frame);
        advanceTriBurstFrame(burst, frame.step);
        const auto expected = row[QStringLiteral("particles")].toArray();
        QCOMPARE(burst.particles.size(), std::size_t(previous + expected.size()));
        for (int i = 0; i < expected.size(); ++i) {
            const auto value = expected[i].toObject();
            const auto &actual = burst.particles[std::size_t(previous+i)];
            for (int axis = 0; axis < 3; ++axis) {
                QCOMPARE(actual.worldMotion.position[std::size_t(axis)], float(value[QStringLiteral("position")].toArray()[axis].toDouble()));
                QCOMPARE(actual.worldMotion.velocity[std::size_t(axis)], float(value[QStringLiteral("velocity")].toArray()[axis].toDouble()));
            }
            QCOMPARE(actual.percentAge, float(value[QStringLiteral("age")].toDouble()));
        }
    }
}

void LogicTests::trailMeshAndPackedColorsMatchNative()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-trail-mesh-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("samples")].toArray();
    QVERIFY(rows.size() >= 8);
    const auto trail = baclickfx::buildSubsystemMap(1, 1, 1080).trail;
    for (const auto &entry : rows) {
        const auto row = entry.toObject();
        std::vector<baclickfx::unity::LinePoint> points;
        for (const auto &p : row[QStringLiteral("points")].toArray()) {
            const auto xy = p.toArray();points.push_back({float(xy[0].toDouble()),float(xy[1].toDouble())});
        }
        baclickfx::unity::LineRibbonScratch scratch;
        std::vector<baclickfx::unity::LineRibbonVertex> vertices;
        baclickfx::unity::buildLineRibbon(points,float(trail.widthMultiplier),4,1,scratch,vertices);
        const auto expected = row[QStringLiteral("vertices")].toArray();
        QCOMPARE(vertices.size(),std::size_t(expected.size()));
        for (int i = 0; i < expected.size(); ++i) {
            const auto value = expected[i].toArray();const auto &v = vertices[std::size_t(i)];
            QCOMPARE(v.position[0],float(value[0].toDouble()));QCOMPARE(v.position[1],float(value[1].toDouble()));
            QCOMPARE(v.u,float(value[2].toDouble()));QCOMPARE(v.v,float(value[3].toDouble()));
            const auto rgba = std::uint32_t(value[4].toDouble());
            const auto color = evalTrailGradientColor(trail,v.u);
            for (int channel = 0; channel < 3; ++channel)
                QCOMPARE(color[std::size_t(channel)],double((rgba>>(channel*8))&255)/255.0);
            QCOMPARE(evalTrailGradientAlpha(trail,v.u),double(rgba>>24)/255.0);
        }
    }
}

void LogicTests::billboardLimitsApplyToNarrowOutputsAndDamageSize()
{
    const auto narrow = baclickfx::buildSubsystemMap(1,5,2160,360);
    const auto wide = baclickfx::buildSubsystemMap(1,5,2160,3840);
    // 源 m_MaxParticleSize=.5 是相对水平视口的边长比例。
    const auto clipped = baclickfx::billboardHalfSize(narrow.ring3,.2f,1.f,0);
    QVERIFY(std::abs(clipped - 90) < 1e-4);
    QVERIFY(baclickfx::billboardHalfSize(wide.ring3,.2f,1.f,0) > clipped);
    QCOMPARE(baclickfx::billboardHalfSize(narrow.ring3,.2f,1.f,100),0.0);
    QCOMPARE(baclickfx::billboardHalfSize(narrow.ring3,0.f,1.f,0),0.0);
}

void LogicTests::unityParametersRetainSerializedPrecisionAndCounts()
{
    const auto map = baclickfx::buildSubsystemMap(1, 1);
    QCOMPARE(map.ring.colorStops.size, std::size_t(2));
    QCOMPARE(map.ring3.colorStops.size, std::size_t(5));
    QCOMPARE(map.ring4.colorStops.size, std::size_t(5));
    QCOMPARE(map.meshTri.colorStops.size, std::size_t(2));
    QCOMPARE(map.ring3.startColor[0], 0.5377358198165894);
    QCOMPARE(map.ring3.startLifetimeScalar, 0.6000000238418579);
    QCOMPARE(map.ring.renderQueue, 4499);
    QCOMPARE(map.meshTri.renderQueue, 4499);
    QCOMPARE(map.trail.renderQueue, 4499);
    QCOMPARE(map.ring3.renderQueue, 4550);
    QCOMPARE(map.ring4.renderQueue, 4550);
    QCOMPARE(map.ring.sizeCurveMultiplier, 2.0);
}

void LogicTests::buttonTriggersDefaultToLeftOnly()
{
    const auto triggers = baclickfx::normalizedTriggerButtons(baclickfx::defaults::kTriggerButtonsDefault);
    for (auto button : {Qt::RightButton, Qt::MiddleButton, Qt::BackButton}) {
        const auto event = baclickfx::mouseButtonTransitions(button, Qt::NoButton, triggers);
        QVERIFY(!event.startsGesture());
        QCOMPARE(event.pressed, Qt::MouseButtons{});
    }
    const auto event = baclickfx::mouseButtonTransitions(Qt::LeftButton, Qt::NoButton, triggers);
    QVERIFY(event.startsGesture());
    QCOMPARE(event.pressed, Qt::MouseButtons(Qt::LeftButton));
}

void LogicTests::buttonChordKeepsOneGestureUntilLastRelease()
{
    const Qt::MouseButtons states[] = {
        Qt::LeftButton,
        Qt::LeftButton, // 移动事件重复上报同一按键状态，不能再次点击。
        Qt::LeftButton | Qt::RightButton,
        Qt::LeftButton | Qt::RightButton | Qt::BackButton,
        Qt::RightButton | Qt::BackButton,
        Qt::RightButton | Qt::MiddleButton | Qt::BackButton,
        Qt::MiddleButton | Qt::BackButton,
        Qt::BackButton, // 最后一个已选按键释放；无关侧键不能维持拖尾。
        Qt::NoButton,
    };
    Qt::MouseButtons previous;
    int starts = 0, ends = 0, clicks = 0;
    for (const auto buttons : states) {
        const auto event = baclickfx::mouseButtonTransitions(
            buttons, previous, baclickfx::kSupportedTriggerButtons);
        starts += event.startsGesture();
        ends += event.endsGesture();
        for (auto button : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton}) {
            clicks += event.pressed.testFlag(button);
        }
        previous = buttons;
    }
    QCOMPARE(starts, 1);
    QCOMPARE(ends, 1);
    QCOMPARE(clicks, 3);
}

void LogicTests::buttonTriggersIgnoreUnsupportedAndEmptySelections()
{
    const auto selected = baclickfx::normalizedTriggerButtons(
        (Qt::RightButton | Qt::MiddleButton | Qt::BackButton).toInt());
    QCOMPARE(selected, Qt::MouseButtons(Qt::RightButton | Qt::MiddleButton));
    const auto together = baclickfx::mouseButtonTransitions(selected, Qt::NoButton, selected);
    QCOMPARE(together.pressed, selected);
    QVERIFY(together.startsGesture());
    const auto disabled = baclickfx::mouseButtonTransitions(
        baclickfx::kSupportedTriggerButtons, Qt::NoButton, baclickfx::normalizedTriggerButtons(0));
    QVERIFY(!disabled.held);
    QCOMPARE(disabled.pressed, Qt::MouseButtons{});
}

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
    stream.advanceTime(0.25);
    QVERIFY(!stream.retireExpired(1.0));
    stream.advanceTime(0.8);
    QVERIFY(stream.retireExpired(1.0));
    QVERIFY(stream.empty());
}

void LogicTests::trailPointsMatchNativeSnapshots()
{
    QFile file(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/tests/fixtures/unity-trail-points-native.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("samples")].toArray();
    QVERIFY(rows.size() >= 18);
    for (const auto &entry : rows) {
        const auto row = entry.toObject();
        const auto input = row[QStringLiteral("input")].toArray();
        const auto expected = row[QStringLiteral("output")].toArray();
        const auto head = row[QStringLiteral("head")].toArray();
        std::vector<baclickfx::unity::TrailPointRecord> points;
        for (const auto &value : input) {
            const auto p = value.toArray();
            points.push_back({{float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())},
                float(p[3].toDouble()), std::uint32_t(p[4].toInt())});
        }
        std::vector<std::array<float, 3>> positions(points.size());
        std::vector<float> flags(points.size());
        baclickfx::unity::prepareTrailPoints(points, 0, points.size(),
            {float(head[0].toDouble()), float(head[1].toDouble()), float(head[2].toDouble())},
            true, float(row[QStringLiteral("now")].toDouble()),
            float(row[QStringLiteral("lifetime")].toDouble()), positions, flags);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto p = expected[int(i)].toArray();
            for (int axis = 0; axis < 3; ++axis)
                QCOMPARE(std::bit_cast<std::uint32_t>(positions[i][std::size_t(axis)]),
                    std::bit_cast<std::uint32_t>(float(p[axis].toDouble())));
            QCOMPARE(flags[i], float(p[3].toDouble()));
        }
    }
}

void LogicTests::trailTailClipsContinuouslyAndHeadStaysLive()
{
    TrailStream stream;
    stream.beginStroke(QPointF(0, 0));
    stream.advanceTime(0.1);
    stream.addSegment(QPointF(0, 0), QPointF(10, 0));
    stream.retireExpired(0.3);
    stream.advanceTime(0.1);
    stream.addSegment(QPointF(10, 0), QPointF(20, 0));
    stream.retireExpired(0.3);
    stream.setHead(QPointF(22, 0));
    stream.advanceTime(0.15);
    QVERIFY(!stream.retireExpired(0.3)); // retain the expired predecessor
    QCOMPARE(stream.points().size(), std::size_t(3));
    baclickfx::Subsystem params;
    params.widthPx = 2;
    const auto strokes = buildTrailStrokes(stream, params);
    QCOMPARE(strokes.size(), std::size_t(1));
    QVERIFY(std::abs(strokes[0].samples.front().pos.x() - 5.0) < 1e-5);
    QCOMPARE(strokes[0].samples.back().pos, QPointF(22, 0));
    QCOMPARE(stream.points().back().pos, QPointF(20, 0));
    stream.advanceTime(0.051);
    QVERIFY(stream.retireExpired(0.3));
    QCOMPARE(stream.points().size(), std::size_t(2));
    stream.advanceTime(0.1);
    QVERIFY(stream.retireExpired(0.3));
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

void LogicTests::pointerPixelRespectsOutputSeams()
{
    const QRect left(-1920, 0, 1920, 1080);
    const QRect center(0, 0, 1280, 720);
    const QRect right(1280, 0, 1920, 1080);
    QVERIFY(left.contains(baclickfx::logicalPixelAt(QPointF(-.25, 10))));
    QVERIFY(center.contains(baclickfx::logicalPixelAt(QPointF(0, 10))));
    QVERIFY(center.contains(baclickfx::logicalPixelAt(QPointF(1279.75, 10))));
    QVERIFY(right.contains(baclickfx::logicalPixelAt(QPointF(1280, 10))));
}

void LogicTests::particleProgressMatchesPercentAge()
{
    QCOMPARE(particleProgress(-1.f), 0.f);
    QCOMPARE(particleProgress(0.f), 0.f);
    QCOMPARE(particleProgress(50.f), .5f);
    QCOMPARE(particleProgress(100.f), 1.f);
    // 源 float32 归一化会把刚超过 100 的百分比再次舍入为 1；退役必须比较原百分比。
    QCOMPARE(particleProgress(100.00000762939453f), 1.f);
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

void LogicTests::meshTriWorldRotationStepsBeforeAgeAdvance()
{
    static const baclickfx::ScalarStop curve[] = {{0.0, 0.0, 0.0, 0.0}, {1.0, 2.0, 0.0, 0.0}};
    MeshTriEmission emission;
    emission.lifetimeSec = 1;
    emission.inverseLifetime = 1;
    emission.params.rotationEnabled = true;
    emission.params.rotationScalar = 1;
    emission.params.rotationCurveMin = curve;
    emission.params.rotationCurveMax = curve;
    emission.arcs.emplace_back();
    // 原生 World 路径先用步进前的年龄求角速度，再推进角度和百分比年龄。
    advanceMeshTri(emission, .5);
    QCOMPARE(emission.arcs[0].angle, 0.0);
    QCOMPARE(emission.percentAge, 50.f);
    advanceMeshTri(emission, .5);
    QCOMPARE(emission.arcs[0].angle, -.5);
    QCOMPARE(emission.percentAge, 100.f);
    advanceMeshTri(emission, .5);
    QCOMPARE(emission.arcs[0].angle, -1.5);
    advanceMeshTri(emission, .5);
    QCOMPARE(emission.arcs[0].angle, -1.5);
}

void LogicTests::bloomPyramidMatchesPpv2Formula()
{
    QCOMPARE(baclickfx::bloomBaseSize(QSize(3200, 2000)), QSize(1600, 1000));
    const auto params = baclickfx::bloomPyramidParams(QSize(1600, 1000));
    QCOMPARE(params.iterations, 7);
    QVERIFY(std::abs(double(params.sampleScale) - 1.143856) < 1e-5);
    QVERIFY(std::abs(baclickfx::bloomReachPx(QSize(3200, 2000), 1.6)
                     - 294.04866) < 1e-4);
    // Include downsampling, upsampling and bilinear sampling footprints.
    // The former 60px estimate for 640x360 visibly clipped the outer halo.
    QVERIFY(baclickfx::bloomReachPx(QSize(640, 360), 1.0) > 100);
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
