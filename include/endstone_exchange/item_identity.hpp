#pragma once

#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/localization.hpp"

#include <endstone/nbt/tag.h>

#include <string>
#include <string_view>

namespace exchange {

[[nodiscard]] bool matchesItemIdentity(const ItemPrototype &prototype, std::string_view item_type, int item_data,
                                       const endstone::CompoundTag &item_nbt);

// Returns the exact player-visible requirements enforced by matchesItemIdentity.
// Keep this in the identity module so the UI and inventory validation cannot drift apart.
[[nodiscard]] std::string describeItemRequirements(
    const ItemPrototype &prototype, Language language = Language::SimplifiedChinese,
    std::string_view localized_item_name = {});

} // namespace exchange
