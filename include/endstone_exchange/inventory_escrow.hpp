#pragma once

#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/item_identity.hpp"

#include <endstone/inventory/inventory.h>
#include <endstone/inventory/item_stack.h>
#include <endstone/inventory/player_inventory.h>

#include <optional>
#include <unordered_map>
#include <vector>

namespace exchange {

struct TaggedSellItems {
    int quantity{0};
    int stacks{0};
};

struct InternalEscrowMarkers {
    std::vector<Id> delivery_claim_ids;
    std::vector<Id> sell_item_escrow_ids;
    std::vector<Id> sell_receipt_escrow_ids;
};

[[nodiscard]] std::vector<endstone::ItemStack> splitStacks(const ItemPrototype &prototype, int quantity,
                                                           std::optional<Id> delivery_claim_id = std::nullopt);
[[nodiscard]] int itemCount(const std::unordered_map<int, endstone::ItemStack> &items);

[[nodiscard]] std::optional<Id> deliveryClaimId(const endstone::ItemStack &item);
[[nodiscard]] bool clearDeliveryClaimTag(endstone::ItemStack &item, Id claim_id,
                                         const ItemPrototype &prototype);
[[nodiscard]] int taggedItemCount(const endstone::PlayerInventory &inventory, Id claim_id);
void clearDeliveryClaimTag(endstone::PlayerInventory &inventory, Id claim_id, const ItemPrototype &prototype);
void removeDeliveryClaimItems(endstone::PlayerInventory &inventory, Id claim_id);

[[nodiscard]] std::optional<Id> sellItemEscrowId(const endstone::ItemStack &item);
[[nodiscard]] std::optional<Id> sellReceiptEscrowId(const endstone::ItemStack &item);
[[nodiscard]] bool isInternalEscrowItem(const endstone::ItemStack &item);
[[nodiscard]] InternalEscrowMarkers internalEscrowMarkers(const endstone::PlayerInventory &inventory);
[[nodiscard]] int sellableItemCount(const endstone::PlayerInventory &inventory, const ItemPrototype &prototype);
[[nodiscard]] TaggedSellItems tagSellItems(endstone::PlayerInventory &inventory, const ItemPrototype &prototype,
                                           int requested_quantity, Id escrow_id);
[[nodiscard]] TaggedSellItems taggedSellItems(const endstone::PlayerInventory &inventory, Id escrow_id);
void restoreTaggedSellItems(endstone::PlayerInventory &inventory, Id escrow_id, const ItemPrototype &prototype);
void removeTaggedSellItems(endstone::PlayerInventory &inventory, Id escrow_id);
void replaceTaggedSellItemsWithReceipts(endstone::PlayerInventory &inventory, Id escrow_id);
[[nodiscard]] int sellReceiptCount(const endstone::PlayerInventory &inventory, Id escrow_id);
void removeSellReceipts(endstone::PlayerInventory &inventory, Id escrow_id);

} // namespace exchange
