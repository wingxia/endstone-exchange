#include "endstone_exchange/inventory_escrow.hpp"

#include "endstone_exchange/nbt_codec.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace exchange {
namespace {

constexpr std::string_view DeliveryClaimTag = "__endstone_exchange_claim";
constexpr std::string_view SellItemTag = "__endstone_exchange_sell_item";
constexpr std::string_view SellReceiptTag = "__endstone_exchange_sell_receipt";
constexpr int OffHandSlot = -1;

std::optional<Id> markerId(const endstone::ItemStack &item, const std::string_view marker) {
    const auto nbt = item.getNbt();
    const auto key = std::string(marker);
    if (!nbt.contains(key) || nbt.at(key).type() != endstone::nbt::Type::String) {
        return std::nullopt;
    }
    const auto value = nbt.at(key).get<endstone::StringTag>().value();
    try {
        std::size_t consumed = 0;
        const auto id = std::stoull(value, &consumed);
        return consumed == value.size() ? std::optional<Id>(id) : std::nullopt;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

void setMarker(endstone::ItemStack &item, const std::string_view marker, const Id id) {
    auto nbt = item.getNbt();
    nbt.insert_or_assign(std::string(marker), endstone::StringTag(std::to_string(id)));
    item.setNbt(nbt);
}

endstone::CompoundTag originalNbt(const ItemPrototype &prototype) {
    return prototype.nbt.empty() ? endstone::CompoundTag{} : NbtCodec::decode(prototype.nbt);
}

endstone::ItemStack sampleItem(const ItemPrototype &prototype) {
    endstone::ItemStack sample(endstone::ItemTypeId(prototype.type), 1, prototype.data);
    sample.setNbt(originalNbt(prototype));
    return sample;
}

endstone::ItemStack sellReceipt(const Id escrow_id) {
    endstone::ItemStack receipt(endstone::ItemTypeId("minecraft:barrier"), 1);
    auto meta = receipt.getItemMeta();
    meta->setDisplayName("§r交易暂存记录");
    meta->setLore(std::vector<std::string>{"§7请勿移动；系统将自动回收"});
    if (!receipt.setItemMeta(meta.get())) {
        throw std::runtime_error("cannot create sell escrow receipt");
    }
    setMarker(receipt, SellReceiptTag, escrow_id);
    return receipt;
}

} // namespace

std::vector<endstone::ItemStack> splitStacks(const ItemPrototype &prototype, const int quantity,
                                             const std::optional<Id> delivery_claim_id) {
    auto sample = sampleItem(prototype);
    if (delivery_claim_id) {
        setMarker(sample, DeliveryClaimTag, *delivery_claim_id);
    }
    const int stack_size = std::max(1, sample.getMaxStackSize());
    std::vector<endstone::ItemStack> result;
    int remaining = quantity;
    while (remaining > 0) {
        auto stack = sample;
        const int amount = std::min(remaining, stack_size);
        stack.setAmount(amount);
        result.push_back(std::move(stack));
        remaining -= amount;
    }
    return result;
}

int itemCount(const std::unordered_map<int, endstone::ItemStack> &items) {
    int result = 0;
    for (const auto &[slot, item] : items) {
        static_cast<void>(slot);
        result += item.getAmount();
    }
    return result;
}

std::optional<Id> deliveryClaimId(const endstone::ItemStack &item) {
    return markerId(item, DeliveryClaimTag);
}

int taggedItemCount(const endstone::Inventory &inventory, const Id claim_id) {
    int result = 0;
    for (const auto &item : inventory.getContents()) {
        if (item && deliveryClaimId(*item) == claim_id) {
            result += item->getAmount();
        }
    }
    return result;
}

void clearDeliveryClaimTag(endstone::Inventory &inventory, const Id claim_id, const ItemPrototype &prototype) {
    const auto nbt = originalNbt(prototype);
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        auto item = inventory.getItem(slot);
        if (item && deliveryClaimId(*item) == claim_id) {
            item->setNbt(nbt);
            inventory.setItem(slot, std::move(item));
        }
    }
}

std::optional<Id> sellItemEscrowId(const endstone::ItemStack &item) {
    return markerId(item, SellItemTag);
}

std::optional<Id> sellReceiptEscrowId(const endstone::ItemStack &item) {
    return markerId(item, SellReceiptTag);
}

bool isInternalEscrowItem(const endstone::ItemStack &item) {
    return deliveryClaimId(item).has_value() || sellItemEscrowId(item).has_value() ||
           sellReceiptEscrowId(item).has_value();
}

int sellableItemCount(const endstone::PlayerInventory &inventory, const ItemPrototype &prototype) {
    int available = 0;
    const auto count = [&](const endstone::ItemStack &item) {
        if (!isInternalEscrowItem(item) &&
            matchesItemIdentity(prototype, static_cast<std::string>(item.getType().getId()), item.getData(),
                                item.getNbt())) {
            available += item.getAmount();
        }
    };
    for (const auto &item : inventory.getContents()) {
        if (item) {
            count(*item);
        }
    }
    if (const auto offhand = inventory.getItemInOffHand()) {
        count(*offhand);
    }
    return available;
}

TaggedSellItems tagSellItems(endstone::PlayerInventory &inventory, const ItemPrototype &prototype,
                             const int requested_quantity, const Id escrow_id) {
    if (requested_quantity <= 0) {
        throw std::runtime_error("sell escrow quantity must be positive");
    }
    std::vector<std::pair<int, endstone::ItemStack>> candidates;
    int available = 0;
    int same_type = 0;
    const auto collect = [&](const int slot, endstone::ItemStack item) {
        if (isInternalEscrowItem(item)) {
            return;
        }
        const auto item_type = static_cast<std::string>(item.getType().getId());
        if (item_type == prototype.type) {
            same_type += item.getAmount();
        }
        if (matchesItemIdentity(prototype, item_type, item.getData(), item.getNbt())) {
            available += item.getAmount();
            candidates.emplace_back(slot, std::move(item));
        }
    };
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        auto item = inventory.getItem(slot);
        if (item) {
            collect(slot, std::move(*item));
        }
    }
    if (auto offhand = inventory.getItemInOffHand()) {
        collect(OffHandSlot, std::move(*offhand));
    }
    if (available < requested_quantity) {
        throw std::runtime_error(std::format(
            "背包中与市场完全一致的 {} 只有 {} 件，需要 {} 件（同类型共 {} 件；名称、附魔、耐久等 NBT 必须一致）",
            prototype.name, available, requested_quantity, same_type));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) { return left.second.getAmount() < right.second.getAmount(); });

    TaggedSellItems result;
    for (auto &[slot, item] : candidates) {
        if (result.quantity >= requested_quantity) {
            break;
        }
        setMarker(item, SellItemTag, escrow_id);
        result.quantity += item.getAmount();
        ++result.stacks;
        if (slot == OffHandSlot) {
            inventory.setItemInOffHand(std::move(item));
        } else {
            inventory.setItem(slot, std::move(item));
        }
    }
    return result;
}

TaggedSellItems taggedSellItems(const endstone::PlayerInventory &inventory, const Id escrow_id) {
    TaggedSellItems result;
    for (const auto &item : inventory.getContents()) {
        if (item && sellItemEscrowId(*item) == escrow_id) {
            result.quantity += item->getAmount();
            ++result.stacks;
        }
    }
    if (const auto item = inventory.getItemInOffHand(); item && sellItemEscrowId(*item) == escrow_id) {
        result.quantity += item->getAmount();
        ++result.stacks;
    }
    return result;
}

void restoreTaggedSellItems(endstone::PlayerInventory &inventory, const Id escrow_id, const ItemPrototype &prototype) {
    const auto nbt = originalNbt(prototype);
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        auto item = inventory.getItem(slot);
        if (item && sellItemEscrowId(*item) == escrow_id) {
            item->setNbt(nbt);
            inventory.setItem(slot, std::move(item));
        }
    }
    auto offhand = inventory.getItemInOffHand();
    if (offhand && sellItemEscrowId(*offhand) == escrow_id) {
        offhand->setNbt(nbt);
        inventory.setItemInOffHand(std::move(*offhand));
    }
}

void replaceTaggedSellItemsWithReceipts(endstone::PlayerInventory &inventory, const Id escrow_id) {
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        const auto item = inventory.getItem(slot);
        if (item && sellItemEscrowId(*item) == escrow_id) {
            inventory.setItem(slot, sellReceipt(escrow_id));
        }
    }
    const auto offhand = inventory.getItemInOffHand();
    if (offhand && sellItemEscrowId(*offhand) == escrow_id) {
        inventory.setItemInOffHand(sellReceipt(escrow_id));
    }
}

int sellReceiptCount(const endstone::PlayerInventory &inventory, const Id escrow_id) {
    int result = 0;
    for (const auto &item : inventory.getContents()) {
        if (item && sellReceiptEscrowId(*item) == escrow_id) {
            result += item->getAmount();
        }
    }
    if (const auto item = inventory.getItemInOffHand(); item && sellReceiptEscrowId(*item) == escrow_id) {
        result += item->getAmount();
    }
    return result;
}

void removeSellReceipts(endstone::PlayerInventory &inventory, const Id escrow_id) {
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        const auto item = inventory.getItem(slot);
        if (item && sellReceiptEscrowId(*item) == escrow_id) {
            inventory.clear(slot);
        }
    }
    if (const auto item = inventory.getItemInOffHand(); item && sellReceiptEscrowId(*item) == escrow_id) {
        inventory.setItemInOffHand(std::nullopt);
    }
}

} // namespace exchange
