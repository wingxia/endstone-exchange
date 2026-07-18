#pragma once

#include "endstone_exchange/domain.hpp"

#include <endstone/nbt/tag.h>

#include <string_view>

namespace exchange {

[[nodiscard]] bool matchesItemIdentity(const ItemPrototype &prototype, std::string_view item_type, int item_data,
                                       const endstone::CompoundTag &item_nbt);

} // namespace exchange
