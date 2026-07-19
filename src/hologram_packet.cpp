#include "endstone_exchange/hologram_packet.hpp"

#include <bit>
#include <cstdint>
#include <string>

namespace exchange {
namespace {

constexpr std::uint32_t EntityDataScale = 38;
constexpr std::uint32_t EntityDataWidth = 53;
constexpr std::uint32_t EntityDataHeight = 54;
constexpr std::uint32_t EntityDataAlwaysShowNameTag = 81;
constexpr std::uint32_t EntityDataTypeByte = 0;
constexpr std::uint32_t EntityDataTypeFloat = 3;
constexpr float HologramScale = 0.01F;

void appendVarUint(std::string &payload, std::uint64_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        payload.push_back(static_cast<char>(byte));
    } while (value != 0);
}

void appendFloat(std::string &payload, const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        payload.push_back(static_cast<char>((bits >> shift) & 0xffU));
    }
}

void appendFloatMetadata(std::string &payload, const std::uint32_t key, const float value) {
    appendVarUint(payload, key);
    appendVarUint(payload, EntityDataTypeFloat);
    appendFloat(payload, value);
}

} // namespace

std::string hologramAppearancePacket(const std::uint64_t runtime_id) {
    std::string payload;
    payload.reserve(32);
    appendVarUint(payload, runtime_id);

    // Entity metadata map.
    appendVarUint(payload, 4);
    appendFloatMetadata(payload, EntityDataScale, HologramScale);
    appendFloatMetadata(payload, EntityDataWidth, 0.0F);
    appendFloatMetadata(payload, EntityDataHeight, 0.0F);
    appendVarUint(payload, EntityDataAlwaysShowNameTag);
    appendVarUint(payload, EntityDataTypeByte);
    payload.push_back(1);

    // Empty integer/float entity-property lists followed by server tick 0.
    appendVarUint(payload, 0);
    appendVarUint(payload, 0);
    appendVarUint(payload, 0);
    return payload;
}

} // namespace exchange
