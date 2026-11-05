// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include "unity/trailpoints.h"
#include "native_oracle.h"
#include <QFile>
#include <QJsonDocument>
#include <cstdio>
#include <vector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile proof(BA_TRAIL_PROOF), dll(SHITTIM_UNITY_PLAYER);
    if (!proof.open(QIODevice::ReadOnly) || !dll.open(QIODevice::ReadOnly)) return 1;
    const auto data = QJsonDocument::fromJson(proof.readAll()).object();
    const auto bytes = dll.readAll();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != data["sha256"].toString().toLatin1()) return 1;
    Native native;
    if (!native.load(data["functions"].toObject(), bytes, QStringLiteral("points"))) return 1;
    using Prepare = void(__attribute__((ms_abi)) *)(void *, void *, void *);
    auto prepare = reinterpret_cast<Prepare>(native.update);
    using baclickfx::unity::TrailPointRecord;
    QJsonArray snapshot;
    int cases = 0, points = 0;
    for (int count : {2, 3, 4, 5, 7, 8, 16, 32}) for (int first : {0, 1, 5, 31})
    for (float spacing : {0.f, .01f, .1f, 1.f}) for (float now : {0.f, .1f, .3f, .35f, .5f, 1.f})
    for (bool emitting : {false, true}) {
        constexpr int capacity = 40;
        std::array<TrailPointRecord, capacity> ring{};
        for (int i = 0; i < capacity; ++i) {
            auto &point = ring[std::size_t((first + i) % capacity)];
            point.position = {float(i) * .14f, float((i % 5) - 2) * .37f, float(i % 3) * .11f};
            point.time = float(i) * spacing;
            point.emitting = i % 3 ? 1 : 0;
        }
        const std::array<float, 3> head{2.37f, -3.4f, .25f};
        const float lifetime = .30000001192092896f;
        alignas(16) unsigned char context[256]{}, storage[64]{};
        put(storage, 8, ring.data()); put(storage, 0x18, std::uint64_t(capacity));
        put(context, 0x90, count); put(context, 0x94, first);
        put(context, 0x98, static_cast<unsigned char *>(storage));
        put(context, 0xa0, head); put(context, 0xac, now); put(context, 0xb0, lifetime);
        put(context, 0xb8, std::uint8_t(emitting));
        std::vector<std::array<float, 3>> actual(std::size_t(count + 4)), expected(std::size_t(count + 4));
        std::vector<float> flags(std::size_t(count + 4), -5), expectedFlags(flags);
        prepare(actual.data(), flags.data(), context);
        baclickfx::unity::prepareTrailPoints(ring, std::size_t(first), std::size_t(count), head, emitting,
            now, lifetime, expected, expectedFlags);
        if (std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(actual[0]))
            || std::memcmp(flags.data(), expectedFlags.data(), flags.size() * sizeof(float))) {
            std::fprintf(stderr, "Trail points mismatch count=%d first=%d spacing=%g now=%g emitting=%d\n", count, first, spacing, now, int(emitting));
            return 1;
        }
        if (count == 4 && first == 0 && emitting && spacing > 0) {
            QJsonArray input, output;
            for (int i = 0; i < count; ++i) {
                const auto &point = ring[std::size_t(i)];
                input.append(QJsonArray{double(point.position[0]), double(point.position[1]), double(point.position[2]), double(point.time), int(point.emitting)});
                output.append(QJsonArray{double(actual[std::size_t(i)][0]), double(actual[std::size_t(i)][1]), double(actual[std::size_t(i)][2]), double(flags[std::size_t(i)])});
            }
            snapshot.append(QJsonObject{{"input", input}, {"output", output}, {"head", QJsonArray{double(head[0]), double(head[1]), double(head[2])}},
                {"now", double(now)}, {"lifetime", double(lifetime)}});
        }
        ++cases; points += count;
    }
    QFile output(BA_NATIVE_SNAPSHOT); if (!output.open(QIODevice::WriteOnly)) return 1;
    output.write(QJsonDocument(QJsonObject{{"unityPlayerSha256", data["sha256"]}, {"samples", snapshot}}).toJson(QJsonDocument::Compact));
    std::printf("Original Trail point preparation: %d cases, %d head/tail positions and emission flags; ring wrap and continuous lifetime clipping bit-identical.\n", cases, points);
    Native pruner;
    if (!pruner.load(data["functions"].toObject(), bytes, QStringLiteral("prune"))) return 1;
    using Prune = void(__attribute__((ms_abi)) *)(void *, float, std::uint32_t);
    auto prune = reinterpret_cast<Prune>(pruner.update);
    int retirements = 0;
    for (int count : {0, 1, 2, 3, 7, 16, 32}) for (int first : {0, 1, 5, 31})
    for (float spacing : {0.f, .01f, .1f, 1.f}) for (float now : {0.f, .1f, .3f, .30000004f, .35f, .5f, 1.f, 4.f}) {
        constexpr int capacity = 40;
        std::array<TrailPointRecord, capacity> ring{};
        for (int i = 0; i < capacity; ++i) ring[std::size_t((first + i) % capacity)].time = float(i) * spacing;
        alignas(16) unsigned char object[640]{}, storage[64]{};
        put(storage, 8, ring.data()); put(storage, 0x18, std::uint64_t(capacity));
        put(object, 0x1d0, static_cast<unsigned char *>(storage)); put(object, 0x1dc, first);
        put(object, 0x1e0, count); put(object, 0x208, .30000001192092896f);
        prune(object, now, 0);
        int nativeFirst = 0, nativeCount = 0;
        std::memcpy(&nativeFirst, object + 0x1dc, 4); std::memcpy(&nativeCount, object + 0x1e0, 4);
        const auto removed = baclickfx::unity::expiredTrailPrefix(std::size_t(count), now, .30000001192092896f,
            [&](std::size_t index) { return ring[(std::size_t(first) + index) % capacity].time; });
        if (nativeFirst != (first + int(removed)) % capacity || nativeCount != count - int(removed)) {
            std::fprintf(stderr, "Trail retirement mismatch count=%d first=%d spacing=%g now=%g: native %d/%d, port removed=%zu\n", count, first, spacing, now, nativeFirst, nativeCount, removed);
            return 1;
        }
        ++retirements;
    }
    std::printf("Original Trail retirement: %d cases, retained old knot, exact boundary and ring indices match; autoDestruct disabled.\n", retirements);
}
