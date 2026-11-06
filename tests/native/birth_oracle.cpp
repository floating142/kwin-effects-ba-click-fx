// SPDX-License-Identifier: GPL-3.0-or-later
// Loader is supplied from the external, hash-pinned native reference checkout.
#include "birth_native_loader.h"
#include "particles/clickinstance.h"
#include "transform_native_loader.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile proof(SHITTIM_BIRTH_PROOF), dllFile(SHITTIM_UNITY_PLAYER);
    if (!proof.open(QIODevice::ReadOnly) || !dllFile.open(QIODevice::ReadOnly)) return 1;
    const auto data = QJsonDocument::fromJson(proof.readAll()).object();
    const auto dll = dllFile.readAll();
    if (QCryptographicHash::hash(dll, QCryptographicHash::Sha256).toHex()
        != data["sha256"].toString().toLatin1()) return 1;
    Native native;
    if (!native.load(data["functions"].toObject(), dll)) return 1;
    const auto map = baclickfx::buildSubsystemMap(1, 1, 1080);
    QFile transformProof(SHITTIM_TRANSFORM_PROOF);
    if (!transformProof.open(QIODevice::ReadOnly)) return 1;
    transform_reference::Native transform;
    if (!transform.load(QJsonDocument::fromJson(transformProof.readAll()).object()["functions"].toObject(), dll)) return 1;
    QJsonArray snapshot, frameSnapshot;
    int cases = 0, particles = 0;
    for (int system : {3, 4}) for (std::uint32_t seed : {0u, 1u, 42u, 0xffffffffu})
    for (int count : {1, 2, 3, 4, 5, 7, 10, 16, 31, 50})
    for (float dt : {0.f, 1.f / 60, .1f, .5f}) for (int previous : {0,1,2,3,7})
    for (bool moving : {false,true}) for (float carry : {0.f,.25f,.99999994f}) for (bool whole : {false,true}) {
        const auto &params = system == 3 ? map.ring3 : map.ring4;
        alignas(16) unsigned char module[8192]{}, owner[128]{}, common[128]{}, context[1024]{}, arrays[2048]{};
        alignas(16) std::array<std::array<float, 264>, 40> output{};
        std::array<void *, 16> job{owner, common, context};
        put(owner, 0x40, static_cast<void *>(common));
        put(owner, 0x48, static_cast<void *>(context));
        put(owner, 0x50, static_cast<void *>(module));
        put(common, 0x30, seed);
        put(common, 0x28, 2.f); put(common, 0x2c, 1.f);
        put(common, 0x48, std::uint32_t(1)); // World simulation.
        put(common, 0x4c, std::uint32_t(1)); // Local scaling, not Hierarchy.
        put(module, 0x174, std::int32_t(params.maxParticles));
        const float scale = float(params.localScale);
        const std::array<float, 16> matrix{scale,0,0,0,0,scale,0,0,0,0,scale,0,0,0,0,1};
        // Build the real World/Local-scaling context from a two-node hierarchy.
        alignas(16) unsigned char cache[128]{}, access[16]{};
        std::array<std::array<float,12>,2> nodes{{
            {0,0,0,0,0,0,0,1,2,2,2,0},
            {0,0,0,0,0,0,0,1,scale,scale,scale,0}}};
        const std::array<int,2> parents{-1,0};
        put(cache, 0x18, nodes.data()); put(cache, 0x20, parents.data());
        put(access, 0, static_cast<void *>(cache)); put(access, 8, std::uint64_t(1));
        transform.build(owner, 0, access, access); // Circle selects transform source 0 (ad6420); distinct from simulationSpace.
        if (std::memcmp(context + 0x8c, matrix.data(), 64)) {
            std::fprintf(stderr, "FX_Touch World/Local context mismatch\n"); return 1;
        }
        std::memcpy(context + 0x8c, matrix.data(), 64);
        std::memcpy(context + 0xcc, matrix.data(), 64);
        put(context, 0x148, shittimParticle::ShapeVector{1,1,1});
        for (const auto &[offset, range] : std::array<std::pair<int, shittimParticle::Range>, 4>{{
                {0x10, {float(params.startLifetimeMinScalar), float(params.startLifetimeScalar), params.startLifetimeMode}},
                {0x30, {float(params.startSpeedMinScalar), float(params.startSpeedScalar), params.startSpeedMode}},
                {0x90, {float(params.startSizeMinScalar), float(params.startSizeScalar), params.startSizeMode}},
                {0x130, {float(params.startRotationMinScalar), float(params.startRotationScalar), params.startRotationMode}}}}) {
            put(module, offset + 4, std::uint16_t(range.mode));
            put(module, offset + 8, range.low);
            put(module, offset + 12, range.high);
        }
        auto *shape = module + 0x210;
        put(shape, 8, std::uint8_t(1)); put(shape, 0x10, 10);
        put(shape, 0x18, 1.f); put(shape, 0x70, 360.f);
        put(shape, 0xc0, shittimParticle::ShapeVector{float(params.shapeScale),float(params.shapeScale),1});
        put(shape, 0x140, 1.f); put(shape, 0x144, 1.f);
        put(module, 0x1d0, std::uint32_t(0xffffffff));
        for (int offset : {0x1d4,0x1d8,0x1dc,0x1f8}) put(module, offset, 1.f);
        put(module, 0x1fc, -1.f);
        for (int channel = 0; channel < 4; ++channel) put(module, 0x78 + channel * 4, 1.f);
        for (int offset = 0; offset <= 0x4c0; offset += 0x20) {
            put(arrays, offset, output[std::size_t(offset / 0x20)].data());
            put(arrays, offset + 0x10, std::uint64_t(previous));
            put(arrays, offset + 0x18, std::uint64_t(264 * 2));
        }
        native.seed(owner);
        KWin::ParticleEmitterState state(seed);
        baclickfx::Rng entropy(0);
        KWin::ParticleBirthFrame frame;
        frame.step = dt;
        frame.offset = whole ? dt : 0;
        frame.rateCount = moving ? std::uint64_t(count) : 0;
        frame.emission.intervalScale = 1.f / (float(count) + carry);
        frame.emission.carry = carry;
        frame.emitterVelocity = moving ? baclickfx::unity::MotionVector{2,-1,.25f}
                                      : baclickfx::unity::MotionVector{};
        put(context, 0x13c, frame.emitterVelocity);
        put(context, 0x1d8, frame.emission.intervalScale); put(context, 0x1dc, carry);
        KWin::TriBurstEmission burst;
        burst.particles.resize(std::size_t(previous));
        KWin::appendTriBurst(burst, params, 1, std::uint64_t(count), entropy, state, frame);
        native.frame(job.data(), arrays, .4f, dt, frame.rateCount, std::uint64_t(count), 0.f, frame.offset);
        std::uint64_t length=0; std::memcpy(&length,arrays+0x10,8);
        if(whole && dt>0) {
            const float tick=baclickfx::unity::simulationStep(dt,.029999999329447746f);
            baclickfx::unity::catchup(dt,tick,1.f,false,[&](float step){
                alignas(16) float durations[4]{step,step,step,step};
                native.step(job.data(),arrays,0,&length,durations,nullptr);
                put(arrays,0x10,length);
            });
            KWin::advanceTriBurstFrame(burst,dt);
        }
        if(length!=burst.particles.size()) {
            std::fprintf(stderr,"FX_Touch birth capacity mismatch %llu/%zu\n",(unsigned long long)length,burst.particles.size());return 1;
        }
        if(std::memcmp(module+0x190,state.initial.state().data(),64)
            || std::memcmp(module+0x450,state.shape.state().data(),64)) {
            std::fprintf(stderr,"FX_Touch full birth RNG mismatch\n");return 1;
        }
        QJsonArray frameParticles;
        for (int i = previous; i < int(length); ++i) {
            const auto &particle = burst.particles[std::size_t(i)];
            if(std::memcmp(&output[30][std::size_t(i)], &particle.percentAge,4)) {
                std::fprintf(stderr,"FX_Touch birth age mismatch dt=%a lane=%d age=%a/%a\n",dt,i,
                    output[30][std::size_t(i)],particle.percentAge);return 1;
            }
            QJsonArray position, velocity;
            for (int axis = 0; axis < 3; ++axis) {
                const float p = output[std::size_t(axis)][std::size_t(i)];
                const float v = output[std::size_t(axis + 3)][std::size_t(i)];
                if (std::memcmp(&p, &particle.worldMotion.position[std::size_t(axis)], 4)
                    || std::memcmp(&v, &particle.worldMotion.velocity[std::size_t(axis)], 4)) {
                    std::fprintf(stderr, "FX_Touch birth mismatch Ring%d seed=%u count=%d dt=%g lane=%d axis=%d position=%a/%a velocity=%a/%a\n",
                        system, seed, count, dt, i, axis, p, particle.worldMotion.position[std::size_t(axis)],
                        v, particle.worldMotion.velocity[std::size_t(axis)]);
                    return 1;
                }
                position.append(double(p)); velocity.append(double(v));
            }
            if (dt == 0 && previous == 0 && !moving && carry == 0 && !whole && i < 4) snapshot.append(QJsonObject{{"system", system}, {"seed", double(seed)},
                {"count", count}, {"lane", i}, {"position", position}, {"velocity", velocity}});
            frameParticles.append(QJsonObject{{"position",position},{"velocity",velocity},
                {"age",double(output[30][std::size_t(i)])}});
            ++particles;
        }
        if(seed==42 && previous==3 && moving && carry==.25f && whole && dt>0 && (count==1 || count==5 || count==50))
            frameSnapshot.append(QJsonObject{{"system",system},{"seed",double(seed)},{"count",count},
                {"previous",previous},{"step",double(dt)},{"carry",double(carry)},
                {"interval",double(frame.emission.intervalScale)},{"particles",frameParticles}});
        ++cases;
    }
    QFile destination(BA_NATIVE_SNAPSHOT);
    if (!destination.open(QIODevice::WriteOnly)) return 1;
    destination.write(QJsonDocument(QJsonObject{{"unityPlayerSha256", data["sha256"]},
        {"samples", snapshot},{"frames",frameSnapshot}}).toJson(QJsonDocument::Compact));
    std::printf("Original FX_Touch birth dispatcher: %d cases, %d positions/velocities/ages and array slot order; partial birth time, emitter interpolation, four-lane padding and newborn retirement bit-identical.\n", cases, particles);
}
