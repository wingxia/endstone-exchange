#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "endstone_exchange/core/Models.hpp"

namespace exchange {

std::string normalizeIdentifier(std::string value);
std::string enchantSignature(std::vector<Enchantment> enchants);
std::string sha256Hex(const std::string& input);
std::string productKey(const std::string& item_id, std::vector<Enchantment> enchants);
ItemSnapshot makeSnapshot(
    std::string item_id,
    std::vector<Enchantment> enchants,
    std::int32_t quantity,
    std::vector<std::uint8_t> nbt_blob,
    std::string nbt_summary);

}  // namespace exchange

