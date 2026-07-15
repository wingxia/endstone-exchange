#pragma once

#include <endstone/nbt/tag.h>

#include <cstdint>
#include <span>
#include <vector>

namespace exchange {

class NbtCodec {
public:
    [[nodiscard]] static std::vector<std::uint8_t> encode(const endstone::CompoundTag &tag);
    [[nodiscard]] static endstone::CompoundTag decode(std::span<const std::uint8_t> bytes);
};

}  // namespace exchange
