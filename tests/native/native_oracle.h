// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <cstring>
#include <map>
#include <sys/mman.h>

template<typename T> void put(unsigned char *p, int offset, const T &value)
{
    std::memcpy(p + offset, &value, sizeof(value));
}

struct Native {
    using Update = void(__attribute__((ms_abi)) *)(void *, void *, void *, void *, std::uint64_t, std::uint64_t);
    unsigned char *memory = nullptr;
    Update update = nullptr;
    ~Native() { if (memory) munmap(memory, 131072); }
    bool load(const QJsonObject &functions, const QByteArray &dll, const QString &entryName = QStringLiteral("rotation"),
              const std::map<QString, std::uintptr_t> &hostFunctions = {})
    {
        memory = static_cast<unsigned char *>(mmap(nullptr, 131072, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (memory == MAP_FAILED) { memory = nullptr; return false; }
        memory[0] = 0x0f; memory[1] = 0x0b; // Any unexpected external call traps.
        std::map<QString, unsigned char *> entries, rvas, constants;
        std::map<QString, QByteArray> values;
        size_t codeCursor = 32, constantCursor = 65536;
        for (const auto &[rva, address] : hostFunctions) {
            // Only allocator/service fixtures may be supplied; numerical kernels
            // remain the hash-checked original code. Absolute jump avoids ASLR range limits.
            rvas[rva] = memory + codeCursor;
            memory[codeCursor] = 0x48; memory[codeCursor + 1] = 0xb8;
            put(memory + codeCursor, 2, address);
            memory[codeCursor + 10] = 0xff; memory[codeCursor + 11] = 0xe0;
            codeCursor += 16;
        }
        for (auto it = functions.begin(); it != functions.end(); ++it) {
            const auto f = it.value().toObject();
            const auto code = QByteArray::fromHex(f["bytes"].toString().toLatin1());
            if (code.isEmpty() || codeCursor + size_t(code.size()) >= 65536
                || dll.mid(f["rawOffset"].toInt(), code.size()) != code
                || QCryptographicHash::hash(code, QCryptographicHash::Sha256).toHex() != f["sha256"].toString().toLatin1()) return false;
            entries[it.key()] = memory + codeCursor;
            rvas[f["rva"].toString()] = memory + codeCursor;
            std::memcpy(memory + codeCursor, code.constData(), code.size());
            // Numerical excerpts end at a caller-owned continuation. A return
            // after the verified excerpt lets the register harness regain control.
            const bool returnFromFragment = f["returnFromFragment"].toBool();
            if (returnFromFragment) memory[codeCursor + size_t(code.size())] = 0xc3;
            codeCursor = (codeCursor + size_t(code.size()) + (returnFromFragment ? 1 : 0) + 15) & ~size_t(15);
            for (auto v : f["constants"].toArray()) {
                const auto c = v.toObject();
                const auto key = c["targetRva"].toString();
                const auto bytes = QByteArray::fromHex(c["bytes"].toString().toLatin1());
                auto &previous = values[key];
                if (bytes.isEmpty() || bytes.size() > 16
                    || previous.left(std::min(previous.size(), bytes.size())) != bytes.left(std::min(previous.size(), bytes.size()))) return false;
                if (bytes.size() > previous.size()) previous = bytes;
            }
        }
        for (auto it = functions.begin(); it != functions.end(); ++it) {
            const auto f = it.value().toObject(); auto code = entries[it.key()];
            const auto length = QByteArray::fromHex(f["bytes"].toString().toLatin1()).size();
            for (auto v : f["calls"].toArray()) {
                const auto c = v.toObject(); const int off = c["offset"].toInt();
                if (off < 0 || off + 5 > length || c["size"].toInt() != 5 || code[off] != 0xe8) return false;
                auto target = rvas.count(c["targetRva"].toString()) ? rvas[c["targetRva"].toString()] : memory;
                put(code, off + 1, std::int32_t(target - (code + off + 5)));
            }
            for (auto v : f["branches"].toArray()) {
                const auto c = v.toObject();
                const int off = c["displacementOffset"].toInt(), next = c["relativeTo"].toInt();
                if (off < 0 || off + 4 > length || next < 0 || next > length) return false;
                auto target = rvas.count(c["targetRva"].toString()) ? rvas[c["targetRva"].toString()] : memory;
                put(code, off, std::int32_t(target - (code + next)));
            }
            for (auto v : f["constants"].toArray()) {
                const auto c = v.toObject(); const auto key = c["targetRva"].toString();
                const int off = c["displacementOffset"].toInt(), relative = c["relativeTo"].toInt();
                if (off < 0 || off + 4 > length || relative < 0 || relative > length) return false;
                if (!constants.count(key)) {
                    if (constantCursor + 16 >= 131072) return false;
                    constants[key] = memory + constantCursor;
                    std::memcpy(constants[key], values[key].constData(), values[key].size());
                    constantCursor += 16;
                }
                put(code, off, std::int32_t(constants[key] - (code + relative)));
            }
        }
        if (mprotect(memory, 65536, PROT_READ | PROT_EXEC)
            || mprotect(memory + 65536, 65536, PROT_READ)) return false;
        update = reinterpret_cast<Update>(entries[entryName]);
        return update;
    }
};
