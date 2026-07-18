#include "endstone_exchange/item_identity.hpp"

#include "endstone_exchange/nbt_codec.hpp"

namespace exchange {

bool matchesItemIdentity(const ItemPrototype &prototype, const std::string_view item_type, const int item_data,
                         const endstone::CompoundTag &item_nbt) {
    const auto expected_nbt = prototype.nbt.empty() ? endstone::CompoundTag{} : NbtCodec::decode(prototype.nbt);
    return item_type == prototype.type && item_data == prototype.data && item_nbt == expected_nbt;
}

} // namespace exchange
