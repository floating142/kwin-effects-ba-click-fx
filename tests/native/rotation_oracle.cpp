// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in only: relocate hash-verified native routines from the user's local DLL.
#include "unity/rotation.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
#include <cstdio>
#include <map>
#include <sys/mman.h>

#include "native_oracle.h"

struct Key { float t, v, inSlope, outSlope; std::uint32_t weightedMode; float inWeight, outWeight; };
static_assert(sizeof(Key) == 28);

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile proof(BA_ROTATION_PROOF), dll(SHITTIM_UNITY_PLAYER), parameters(BA_TOUCH_SOURCE);
    if (!proof.open(QIODevice::ReadOnly) || !dll.open(QIODevice::ReadOnly) || !parameters.open(QIODevice::ReadOnly)) return 1;
    const auto data = QJsonDocument::fromJson(proof.readAll()).object(); const auto bytes = dll.readAll();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != data["sha256"].toString().toLatin1()) return 1;
    Native native; if (!native.load(data["functions"].toObject(), bytes)) return 1;
    const auto source = QJsonDocument::fromJson(parameters.readAll()).object()["particles"].toObject()["MeshTri"].toObject()["RotationModule"].toObject()["curve"].toObject();
    std::array<std::vector<Key>, 2> keys;
    alignas(16) unsigned char config[128]{}, minimum[128]{}, maximum[128]{}, arrays[2048]{};
    for (int i = 0; i < 2; ++i) {
        const auto curve = source[i == 0 ? "minCurve" : "maxCurve"].toObject();
        for (auto entry : curve["m_Curve"].toArray()) {
            const auto k = entry.toObject();
            keys[size_t(i)].push_back({float(k["time"].toDouble()), float(k["value"].toDouble()),
                float(k["inSlope"].toDouble()), float(k["outSlope"].toDouble()), 0, 0, 0});
        }
        auto memory = i == 0 ? minimum : maximum;
        put(memory, 0, -1); put(memory, 4, -std::numeric_limits<float>::infinity()); put(memory, 8, -std::numeric_limits<float>::infinity());
        put(memory, 0x38, keys[size_t(i)].data()); put(memory, 0x48, std::uint64_t(keys[size_t(i)].size()));
        put(memory, 0x58, 2); put(memory, 0x5c, 2);
    }
    const float scalar = float(source["scalar"].toDouble());
    put(config, 4, std::uint16_t(2)); put(config, 0xc, scalar);
    put(config, 0x10, static_cast<unsigned char *>(minimum)); put(config, 0x18, static_cast<unsigned char *>(maximum));
    alignas(16) std::array<std::uint32_t, 4> seeds{};
    alignas(16) std::array<float, 4> ages{}, output{}, flips{};
    put(arrays, 0x380, seeds.data()); put(arrays, 0x3c0, ages.data());
    QJsonArray snapshot; int count = 0;
    for (std::uint32_t base = 0; base < 65536; base += 128) for (int time = 0; time <= 1024; time += 4) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            seeds[lane] = base + lane * 0x9e3779b9u;
            ages[lane] = float(time + int(lane)) * (100.f / 1024.f);
        }
        output.fill(0); native.update(config, arrays, output.data(), flips.data(), 0, 4);
        for (unsigned lane = 0; lane < 4; ++lane) {
            volatile float progress = std::max(0.f, ages[lane] * .01f);
            const float expected = baclickfx::unity::rotationSpeed(keys[0], keys[1], scalar, progress, seeds[lane]);
            if (std::memcmp(&expected, &output[lane], 4)) {
                std::fprintf(stderr, "Rotation mismatch seed=%u age=%g native=%a port=%a\n", seeds[lane], float(progress), output[lane], expected); return 1;
            }
            if (base == 0 && time % 32 == 0) snapshot.append(QJsonObject{{"seed", double(seeds[lane])}, {"progress", double(progress)}, {"speed", double(output[lane])}});
            ++count;
        }
    }
    QFile destination(BA_NATIVE_SNAPSHOT); if (!destination.open(QIODevice::WriteOnly)) return 1;
    destination.write(QJsonDocument(QJsonObject{{"unityPlayerSha256", data["sha256"]}, {"samples", snapshot}}).toJson(QJsonDocument::Compact));
    std::printf("Original World RotationModule: %d bit-identical angular speeds, source TwoCurves/scalar and per-particle mix/flip streams; pipeline precedes integration.\n", count);
}
