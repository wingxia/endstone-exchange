#include "endstone_exchange/config.hpp"
#include "endstone_exchange/database.hpp"
#include "endstone_exchange/exchange_service.hpp"
#include "endstone_exchange/hologram_packet.hpp"
#include "endstone_exchange/inventory_escrow.hpp"
#include "endstone_exchange/interaction_gate.hpp"
#include "endstone_exchange/item_identity.hpp"
#include "endstone_exchange/localization.hpp"
#include "endstone_exchange/market_display.hpp"
#include "endstone_exchange/nbt_codec.hpp"
#include "endstone_exchange/structure_reader.hpp"
#include "endstone_exchange/trade_form.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error("test failed: " + message);
    }
}

void testNbtCodec() {
    endstone::CompoundTag nested;
    nested.insert_or_assign("answer", endstone::IntTag(42));
    nested.insert_or_assign("name", endstone::StringTag("exchange"));

    endstone::ListTag list;
    list.emplace_back(endstone::LongTag(10));
    list.emplace_back(endstone::LongTag(-20));

    endstone::CompoundTag original;
    original.insert_or_assign("byte", endstone::ByteTag(255));
    original.insert_or_assign("short", endstone::ShortTag(-1234));
    original.insert_or_assign("int", endstone::IntTag(-1234567));
    original.insert_or_assign("long", endstone::LongTag(9'876'543'210LL));
    original.insert_or_assign("float", endstone::FloatTag(1.25F));
    original.insert_or_assign("double", endstone::DoubleTag(-4.5));
    original.insert_or_assign("bytes", endstone::ByteArrayTag({0, 1, 127, 255}));
    original.insert_or_assign("ints", endstone::IntArrayTag({1, -2, 3}));
    original.insert_or_assign("list", std::move(list));
    original.insert_or_assign("nested", std::move(nested));

    const auto encoded = exchange::NbtCodec::encode(original);
    const auto decoded = exchange::NbtCodec::decode(encoded);
    require(decoded == original, "NBT round trip must preserve every value");

    bool rejected = false;
    try {
        auto truncated = encoded;
        truncated.pop_back();
        static_cast<void>(exchange::NbtCodec::decode(truncated));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "truncated NBT must be rejected");
}

void testDeliveryMarkerCleanup() {
    endstone::CompoundTag enchantment;
    enchantment.insert_or_assign("id", endstone::ShortTag(3));
    enchantment.insert_or_assign("lvl", endstone::ShortTag(4));
    endstone::ListTag enchantments;
    enchantments.emplace_back(std::move(enchantment));

    endstone::CompoundTag tagged;
    tagged.insert_or_assign("ench", std::move(enchantments));
    tagged.insert_or_assign("__endstone_exchange_claim", endstone::StringTag("57"));

    require(exchange::clearDeliveryClaimTag(tagged, 57), "matching delivery marker must be removed");
    require(!tagged.contains("__endstone_exchange_claim") && tagged.contains("ench"),
            "delivery cleanup must preserve the live enchanted-item NBT");

    tagged.insert_or_assign("__endstone_exchange_claim", endstone::StringTag("58"));
    require(!exchange::clearDeliveryClaimTag(tagged, 57) && tagged.contains("__endstone_exchange_claim"),
            "delivery cleanup must not remove a different claim marker");

    endstone::CompoundTag frame_sale_item;
    frame_sale_item.insert_or_assign("custom", endstone::StringTag("kept"));
    exchange::tagFrameSaleItem(frame_sale_item, 91);
    require(exchange::frameSaleItemId(frame_sale_item) == 91,
            "frame-sale drop marker must survive on the physical item");
    require(exchange::clearFrameSaleItemTag(frame_sale_item, 91) &&
                !exchange::frameSaleItemId(frame_sale_item).has_value() && frame_sale_item.contains("custom"),
            "frame-sale marker cleanup must preserve a usable purchased item");
}

void testConfig() {
    const auto path = std::filesystem::temp_directory_path() / "endstone-exchange-config-test.toml";
    exchange::Config::writeTemplate(path);
    auto config = exchange::Config::load(path);
    require(config.database.host == "127.0.0.1", "template host");
    require(config.market.initial_balance_cents == 1'000'000, "money parsing");
    require(config.market.max_order_quantity == 640, "default maximum order quantity");
    require(config.market.price_min_cents == 100 && config.market.price_max_cents == 500'000 &&
                config.market.price_step_cents == 100,
            "whole-u price range parsing");
    require(config.market.frame_capture_delay_ticks == 80, "item-frame capture delay");
    require(config.economy.provider == "internal" && config.economy.unit_cents == 100,
            "internal economy defaults");
    config.economy.provider = "umoney";
    config.economy.bridge_token = "0123456789abcdef0123456789abcdef";
    config.market.initial_balance_cents = 0;
    config.validate();
    bool rejected_umoney_mint = false;
    try {
        config.market.initial_balance_cents = 100;
        config.validate();
    } catch (const std::exception &) {
        rejected_umoney_mint = true;
    }
    require(rejected_umoney_mint, "UMoney mode must reject minted initial balances");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void testTradeFormFlow() {
    using exchange::TradeAction;

    const auto empty = exchange::availableTradeActions(false, false);
    require(empty == std::vector<TradeAction>{TradeAction::LimitBuy, TradeAction::LimitSell},
            "empty order books must hide both direct trade actions");

    const auto bids_only = exchange::availableTradeActions(true, false);
    require(bids_only ==
                std::vector<TradeAction>{TradeAction::MarketSell, TradeAction::LimitBuy, TradeAction::LimitSell},
            "a buy order must expose direct sell only");

    const auto asks_only = exchange::availableTradeActions(false, true);
    require(asks_only ==
                std::vector<TradeAction>{TradeAction::MarketBuy, TradeAction::LimitBuy, TradeAction::LimitSell},
            "a sell order must expose direct buy only");

    const auto both = exchange::availableTradeActions(true, true);
    require(both == std::vector<TradeAction>{TradeAction::MarketBuy, TradeAction::MarketSell,
                                             TradeAction::LimitBuy, TradeAction::LimitSell},
            "available direct actions must be first and limit actions must remain available");
    require(exchange::tradeActionUsesPrice(TradeAction::LimitBuy) &&
                exchange::tradeActionUsesPrice(TradeAction::LimitSell) &&
                !exchange::tradeActionUsesPrice(TradeAction::MarketBuy) &&
                !exchange::tradeActionUsesPrice(TradeAction::MarketSell),
            "direct trades must never have a price input");
    require(exchange::tradeActionSide(TradeAction::LimitBuy) == exchange::Side::Buy &&
                exchange::tradeActionSide(TradeAction::MarketBuy) == exchange::Side::Buy &&
                exchange::tradeActionSide(TradeAction::LimitSell) == exchange::Side::Sell &&
                exchange::tradeActionSide(TradeAction::MarketSell) == exchange::Side::Sell,
            "trade action side mapping");
    require(exchange::tradeActionOrderType(TradeAction::LimitBuy) == exchange::OrderType::Limit &&
                exchange::tradeActionOrderType(TradeAction::MarketSell) == exchange::OrderType::Market,
            "trade action order type mapping");
    require(exchange::defaultTradePrice(100, 500'000, 100, 1'049) == 1'000 &&
                exchange::defaultTradePrice(100, 500'000, 100, 1'050) == 1'100 &&
                exchange::defaultTradePrice(100, 500'000, 100, 900'000) == 500'000,
            "default input price must snap to the configured whole-u range");

    const auto limit_values = exchange::textFormValues(R"(["640","5000"])");
    require(limit_values == std::vector<std::string>{"640", "5000"},
            "limit form must accept quantity and whole-u price text");
    const auto direct_values = exchange::textFormValues(R"(["7"])");
    require(direct_values == std::vector<std::string>{"7"},
            "direct form response must contain quantity only");
    require(exchange::parseTradeQuantity(" 640 ", 640) == 640, "quantity input trims surrounding whitespace");
    require(exchange::parseTradePrice("5000", 100, 500'000, 100) == 500'000,
            "whole-u input maps to exact cents");

    bool rejected = false;
    try {
        static_cast<void>(exchange::textFormValues(R"([1])"));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "numeric JSON must not bypass text input validation");
    for (const auto malformed : {R"(["1",)", R"(["1"] trailing)"}) {
        rejected = false;
        try {
            static_cast<void>(exchange::textFormValues(malformed));
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected, "malformed text input JSON must be rejected");
    }
    rejected = false;
    try {
        static_cast<void>(exchange::parseTradeQuantity("641", 640));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "quantity input must reject values above 640");
    rejected = false;
    try {
        static_cast<void>(exchange::parseTradePrice("10.5", 100, 500'000, 100));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "price input must reject decimal values");
    rejected = false;
    try {
        static_cast<void>(exchange::parseTradePrice("5001", 100, 500'000, 100));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "price input must reject values above 5000u");

    exchange::OrderBook compact_book;
    compact_book.bids = {{1'000, 90}, {900, 20}, {800, 5}};
    compact_book.asks = {{1'100, 4}};
    const auto overview = exchange::tradeOverviewText(compact_book, "9900u", 64, {});
    require(overview ==
                "§a有人收购§r  10u x 90  ·  9u x 20  ·  …\n"
                "§c有人出售§r  11u x 4\n"
                "§6余额§r §e9900u§r  ·  §6可卖§r §e64 件§r",
            "trade overview must fit the book and account state into three compact lines");
    require(overview.find("\n\n") == std::string::npos, "compact trade overview must not contain blank lines");

    const exchange::TradeDraft limit_buy{TradeAction::LimitBuy, 5, 1'000};
    const auto limit_review = exchange::tradeReviewText(limit_buy);
    require(limit_review.find("数量：5 件") != std::string::npos &&
                limit_review.find("每件价格：10u") != std::string::npos &&
                limit_review.find("最多支付：50u") != std::string::npos,
            "limit confirmation must show quantity, whole-u price and maximum payment");
    const exchange::TradeDraft direct_sell{TradeAction::MarketSell, 7, 0};
    const auto direct_review = exchange::tradeReviewText(direct_sell);
    require(direct_review.find("数量：7 件") != std::string::npos &&
                direct_review.find("每件价格") == std::string::npos,
            "direct confirmation must stay compact and hide price settings");
}

void testLocalization() {
    using exchange::Language;
    using exchange::Message;
    require(exchange::translationCatalogComplete(), "every supported language must contain every message");
    for (const auto language : {Language::SimplifiedChinese, Language::English, Language::TraditionalChinese,
                                Language::Japanese}) {
        for (std::size_t index = 0; index < static_cast<std::size_t>(Message::Count); ++index) {
            const auto pattern = exchange::messageText(language, static_cast<Message>(index));
            static_cast<void>(std::vformat(pattern, std::make_format_args("1", "2", "3", "4", "5", "6")));
        }
    }
    require(exchange::languageFromLocale("zh_CN") == Language::SimplifiedChinese &&
                exchange::languageFromLocale("zh-CN") == Language::SimplifiedChinese &&
                exchange::languageFromLocale("en_GB") == Language::English &&
                exchange::languageFromLocale("zh_TW") == Language::TraditionalChinese &&
                exchange::languageFromLocale("zh_HK") == Language::TraditionalChinese &&
                exchange::languageFromLocale("ja_JP") == Language::Japanese &&
                exchange::languageFromLocale("ko_KR") == Language::SimplifiedChinese,
            "Minecraft client locale mapping and fallback");
    require(exchange::localeCode(Language::SimplifiedChinese) == "zh_CN" &&
                exchange::localeCode(Language::English) == "en_US" &&
                exchange::localeCode(Language::TraditionalChinese) == "zh_TW" &&
                exchange::localeCode(Language::Japanese) == "ja_JP",
            "canonical locale codes");

    require(exchange::tradeActionLabel(exchange::TradeAction::MarketBuy, Language::SimplifiedChinese) ==
                "直接购买" &&
                exchange::tradeActionLabel(exchange::TradeAction::MarketBuy, Language::English) == "Buy now" &&
                exchange::tradeActionLabel(exchange::TradeAction::MarketBuy, Language::TraditionalChinese) ==
                    "直接購買" &&
                exchange::tradeActionLabel(exchange::TradeAction::MarketBuy, Language::Japanese) == "すぐ購入",
            "trade action labels follow client language");

    exchange::OrderBook localized_book;
    localized_book.bids = {{1'000, 9}};
    localized_book.asks = {{1'100, 4}};
    for (const auto language : {Language::SimplifiedChinese, Language::English, Language::TraditionalChinese,
                                Language::Japanese}) {
        const auto overview = exchange::tradeOverviewText(localized_book, "100u", 3, {}, language);
        require(std::ranges::count(overview, '\n') == 2 && overview.find("\n\n") == std::string::npos,
                "every language must keep a plain-item overview to three non-empty lines");
        const auto review = exchange::tradeReviewText(
            exchange::TradeDraft{exchange::TradeAction::LimitSell, 5, 1'000}, language);
        require(std::ranges::count(review, '\n') == 3 && review.find("10u") != std::string::npos &&
                    review.find("50u") != std::string::npos,
                "every language must keep a limit review to four compact lines with whole-u values");
    }

    const exchange::ItemPrototype item{"minecraft:emerald_block", 0, {}, "Emerald Block"};
    const auto simplified = exchange::describeItemRequirements(item, Language::SimplifiedChinese, "绿宝石块");
    const auto english = exchange::describeItemRequirements(item, Language::English, "Emerald Block");
    const auto traditional = exchange::describeItemRequirements(item, Language::TraditionalChinese, "綠寶石方塊");
    const auto japanese = exchange::describeItemRequirements(item, Language::Japanese, "エメラルドブロック");
    require(simplified.empty() && english.empty() && traditional.empty() && japanese.empty(),
            "plain item defaults must not create requirements in any language");

    for (const auto [language, expected] :
         std::array<std::pair<Language, std::string_view>, 4>{{
             {Language::SimplifiedChinese, "数量必须是 1 至 640 的整数"},
             {Language::English, "Quantity must be a whole number from 1 to 640"},
             {Language::TraditionalChinese, "數量必須是 1 至 640 的整數"},
             {Language::Japanese, "数量は 1～640 の整数で入力してください"},
         }}) {
        std::string error_text;
        try {
            static_cast<void>(exchange::parseTradeQuantity("641", 640, language));
        } catch (const exchange::UserError &error) {
            error_text = error.what();
        }
        require(error_text == expected, "localized quantity validation error");
    }
    const exchange::UserError player_error("visible");
    const std::runtime_error internal_error("database detail");
    require(exchange::userFacingError(Language::English, player_error) == "visible" &&
                exchange::userFacingError(Language::English, internal_error) ==
                    exchange::messageText(Language::English, Message::UnexpectedError),
            "internal errors must not leak while validated errors stay specific");
}

void testInteractionGate() {
    using namespace std::chrono_literals;
    exchange::InteractionGate gate(750ms);
    const auto start = exchange::InteractionGate::Clock::time_point{} + 10s;
    require(gate.accept("player", start), "first interaction must be accepted");
    require(!gate.accept("player", start + 1ms), "duplicate packet in the same click must be rejected");
    require(!gate.accept("player", start + 749ms), "interaction stays blocked for the full cooldown");
    require(gate.accept("player", start + 750ms), "a deliberate later click must be accepted");
    require(gate.accept("other-player", start + 1ms), "players must have independent interaction gates");
    gate.clear();
    require(gate.accept("player", start + 751ms), "clearing plugin state must release the gate");
}

std::uint64_t readVarUint(const std::string_view payload, std::size_t &offset) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    while (offset < payload.size() && shift < 64) {
        const auto byte = static_cast<std::uint8_t>(payload[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    throw std::runtime_error("invalid test varuint");
}

float readLittleFloat(const std::string_view payload, std::size_t &offset) {
    if (payload.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("truncated test float");
    }
    std::uint32_t bits = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset++])) << shift;
    }
    return std::bit_cast<float>(bits);
}

std::string readProtocolString(const std::string_view payload, std::size_t &offset) {
    const auto size = readVarUint(payload, offset);
    if (size > payload.size() - offset) {
        throw std::runtime_error("truncated test string");
    }
    auto result = std::string(payload.substr(offset, static_cast<std::size_t>(size)));
    offset += static_cast<std::size_t>(size);
    return result;
}

void testHologramAppearancePacket() {
    const auto payload = exchange::hologramAppearancePacket(300);
    std::size_t offset = 0;
    require(readVarUint(payload, offset) == 300, "hologram packet runtime id");
    require(readVarUint(payload, offset) == 4, "hologram packet metadata count");

    require(readVarUint(payload, offset) == 38 && readVarUint(payload, offset) == 3,
            "hologram packet scale metadata header");
    require(readLittleFloat(payload, offset) == 0.01F, "hologram actor must be client-side tiny");
    require(readVarUint(payload, offset) == 53 && readVarUint(payload, offset) == 3 &&
                readLittleFloat(payload, offset) == 0.0F,
            "hologram packet zero width");
    require(readVarUint(payload, offset) == 54 && readVarUint(payload, offset) == 3 &&
                readLittleFloat(payload, offset) == 0.0F,
            "hologram packet zero height");
    require(readVarUint(payload, offset) == 81 && readVarUint(payload, offset) == 0 &&
                static_cast<std::uint8_t>(payload.at(offset++)) == 1,
            "hologram packet always-visible nametag");
    require(readVarUint(payload, offset) == 0 && readVarUint(payload, offset) == 0 &&
                readVarUint(payload, offset) == 0 && offset == payload.size(),
            "hologram packet empty properties and tick");

    const auto localized_payload = exchange::hologramAppearancePacket(301, "§aBuying 10u x 5§r");
    offset = 0;
    require(readVarUint(localized_payload, offset) == 301 && readVarUint(localized_payload, offset) == 5,
            "localized hologram packet metadata count");
    require(readVarUint(localized_payload, offset) == 4 && readVarUint(localized_payload, offset) == 4 &&
                readProtocolString(localized_payload, offset) == "§aBuying 10u x 5§r",
            "localized hologram packet must override the name for one client");
}

void testMarketDisplay() {
    require(exchange::formatCurrency(875'300) == "8753u" &&
                exchange::formatCurrency(875'350) == "8753.50u" &&
                exchange::formatCurrency(-50) == "-0.50u" &&
                exchange::formatCurrency(std::numeric_limits<exchange::Cents>::min()) ==
                    "-92233720368547758.08u",
            "currency display must omit only redundant decimals and remain overflow-safe");
    require(exchange::blockToChunk(0) == 0 && exchange::blockToChunk(15) == 0 &&
                exchange::blockToChunk(16) == 1 && exchange::blockToChunk(-1) == -1 &&
                exchange::blockToChunk(-16) == -1 && exchange::blockToChunk(-17) == -2,
            "block coordinates must map to Bedrock chunks using floor division");

    exchange::Market market;
    market.item.name = "Emerald Block";
    exchange::OrderBook book;
    book.bids.push_back({1000, 5});
    book.asks.push_back({1200, 3});
    const auto text = exchange::marketHologramText(market, book);
    require(text.find("有人收购 10u x 5") != std::string::npos &&
                text.find("有人出售 12u x 3") != std::string::npos,
            "floating text must use whole-u prices and quantities");
    require(text.find("右击") == std::string::npos && std::count(text.begin(), text.end(), '\n') == 2,
            "floating text must contain only the item and two price lines");
    for (const auto banned : {"\u76d8\u53e3", "\u4e70\u4e00", "\u5356\u4e00", "\u4e70\u76d8", "\u5356\u76d8"}) {
        require(text.find(banned) == std::string::npos, "floating text must not use stock-market wording");
    }
    require(exchange::marketHologramText(market, book, exchange::Language::English)
                    .find("Someone wants to buy 10u x 5") != std::string::npos &&
                exchange::marketHologramText(market, book, exchange::Language::TraditionalChinese)
                        .find("有人收購 10u x 5") != std::string::npos &&
                exchange::marketHologramText(market, book, exchange::Language::Japanese)
                        .find("購入希望者 10u x 5") != std::string::npos,
            "floating text follows each recipient's language");

    const exchange::HologramAnchor anchor{"Overworld", 10.5F, 65.0F, 20.5F};
    auto moved = anchor;
    moved.y += 1.0F;
    require(!exchange::shouldRepositionHologram(exchange::TargetKind::Block, std::nullopt, anchor) &&
                !exchange::shouldRepositionHologram(exchange::TargetKind::Block, anchor, anchor) &&
                exchange::shouldRepositionHologram(exchange::TargetKind::Block, anchor, moved),
            "a block label may be corrected once but must stay still at its remembered anchor");
    require(exchange::shouldRepositionHologram(exchange::TargetKind::Actor, std::nullopt, anchor) &&
                !exchange::shouldRepositionHologram(exchange::TargetKind::Actor, anchor, anchor) &&
                exchange::shouldRepositionHologram(exchange::TargetKind::Actor, anchor, moved),
            "an actor label must move only when its target anchor changes");

    require(exchange::itemFrameSupportOffset(0) == exchange::BlockOffset{0, 1, 0} &&
                exchange::itemFrameSupportOffset(1) == exchange::BlockOffset{0, -1, 0} &&
                exchange::itemFrameSupportOffset(2) == exchange::BlockOffset{0, 0, 1} &&
                exchange::itemFrameSupportOffset(3) == exchange::BlockOffset{0, 0, -1} &&
                exchange::itemFrameSupportOffset(4) == exchange::BlockOffset{1, 0, 0} &&
                exchange::itemFrameSupportOffset(5) == exchange::BlockOffset{-1, 0, 0} &&
                !exchange::itemFrameSupportOffset(6),
            "item-frame facing must resolve to the block behind the frame");
}

void testCanonicalItemIdentity() {
    endstone::CompoundTag empty;
    const exchange::ItemPrototype plain{"minecraft:emerald_block", 0, exchange::NbtCodec::encode(empty),
                                        "Emerald Block"};
    require(exchange::matchesItemIdentity(plain, "minecraft:emerald_block", 0, empty),
            "a newly acquired plain item must match the stored market identity");
    require(!exchange::matchesItemIdentity(plain, "minecraft:emerald", 0, empty),
            "different item types must not pass inventory verification");

    endstone::CompoundTag named;
    named.insert_or_assign("display", endstone::StringTag("custom"));
    require(!exchange::matchesItemIdentity(plain, "minecraft:emerald_block", 0, named),
            "custom NBT must not be silently exchanged for a plain market item");
    const exchange::ItemPrototype custom{"minecraft:emerald_block", 0, exchange::NbtCodec::encode(named),
                                         "Custom Emerald Block"};
    require(exchange::matchesItemIdentity(custom, "minecraft:emerald_block", 0, named),
            "an exact custom item must pass canonical inventory verification");

    const exchange::ItemPrototype plain_requirements{
        "minecraft:emerald_block", 0, exchange::NbtCodec::encode(endstone::CompoundTag{}), "Emerald Block"};
    const auto plain_text = exchange::describeItemRequirements(plain_requirements);
    require(plain_text.empty(), "plain items must not list absent or zero-valued properties");

    endstone::CompoundTag default_properties;
    default_properties.insert_or_assign("Damage", endstone::IntTag(0));
    default_properties.insert_or_assign("RepairCost", endstone::IntTag(0));
    default_properties.insert_or_assign("Unbreakable", endstone::ByteTag(0));
    default_properties.insert_or_assign("ench", endstone::ListTag{});
    const exchange::ItemPrototype defaults{"minecraft:diamond_pickaxe", 0,
                                           exchange::NbtCodec::encode(default_properties), "Diamond Pickaxe"};
    require(exchange::describeItemRequirements(defaults).empty(),
            "serialized default NBT values must stay hidden from players");

    endstone::CompoundTag display;
    display.insert_or_assign("Name", endstone::StringTag("§a矿工镐"));
    endstone::ListTag lore;
    lore.emplace_back(endstone::StringTag("第一行"));
    lore.emplace_back(endstone::StringTag("第二行"));
    display.insert_or_assign("Lore", std::move(lore));

    endstone::ListTag enchantments;
    endstone::CompoundTag efficiency;
    efficiency.insert_or_assign("id", endstone::ShortTag(15));
    efficiency.insert_or_assign("lvl", endstone::ShortTag(5));
    enchantments.emplace_back(std::move(efficiency));
    endstone::CompoundTag unbreaking;
    unbreaking.insert_or_assign("id", endstone::ShortTag(17));
    unbreaking.insert_or_assign("lvl", endstone::ShortTag(3));
    enchantments.emplace_back(std::move(unbreaking));

    endstone::ListTag can_destroy;
    can_destroy.emplace_back(endstone::StringTag("minecraft:stone"));
    can_destroy.emplace_back(endstone::StringTag("minecraft:deepslate"));

    endstone::CompoundTag detailed;
    detailed.insert_or_assign("display", std::move(display));
    detailed.insert_or_assign("ench", std::move(enchantments));
    detailed.insert_or_assign("Damage", endstone::IntTag(12));
    detailed.insert_or_assign("RepairCost", endstone::IntTag(7));
    detailed.insert_or_assign("Unbreakable", endstone::ByteTag(1));
    detailed.insert_or_assign("CanDestroy", std::move(can_destroy));
    detailed.insert_or_assign("custom_origin", endstone::StringTag("village_reward"));
    const exchange::ItemPrototype detailed_item{"minecraft:diamond_pickaxe", 4, exchange::NbtCodec::encode(detailed),
                                                "矿工镐"};
    const auto details = exchange::describeItemRequirements(detailed_item);
    require(
        details.find("数据值：4") != std::string::npos && details.find("自定义名称：§a矿工镐§r") != std::string::npos &&
            details.find("物品说明：第一行§r；第二行§r") != std::string::npos &&
            details.find("附魔：效率 5、耐久 3") != std::string::npos &&
            details.find("耐久损耗：12") != std::string::npos && details.find("铁砧修复代价：7") != std::string::npos &&
            details.find("不会损坏：是") != std::string::npos &&
            details.find("可破坏方块：minecraft:stone、minecraft:deepslate") != std::string::npos &&
            details.find("custom_origin：village_reward") != std::string::npos,
        "special item requirements must name enchantments and every other matching field");
    require(details.find("NBT") == std::string::npos && details.find("（") == std::string::npos,
            "player-facing requirements must avoid technical or parenthetical shorthand");
}

void appendLittle16(std::vector<std::uint8_t> &bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLittle32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendLittle64(std::vector<std::uint8_t> &bytes, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendVarUint(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        bytes.push_back(byte);
    } while (value != 0);
}

void appendNbtString(std::vector<std::uint8_t> &bytes, const std::string_view value) {
    appendLittle16(bytes, static_cast<std::uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendNamedTag(std::vector<std::uint8_t> &bytes, const std::uint8_t type, const std::string_view name) {
    bytes.push_back(type);
    appendNbtString(bytes, name);
}

std::vector<std::uint8_t> frameStructureNbt() {
    std::vector<std::uint8_t> bytes;
    appendNamedTag(bytes, 10, "");
    appendNamedTag(bytes, 10, "structure");
    appendNamedTag(bytes, 10, "block_position_data");
    appendNamedTag(bytes, 10, "0");
    appendNamedTag(bytes, 10, "block_entity_data");
    appendNamedTag(bytes, 10, "Item");
    appendNamedTag(bytes, 1, "Count");
    bytes.push_back(1);
    appendNamedTag(bytes, 2, "Damage");
    appendLittle16(bytes, 0);
    appendNamedTag(bytes, 8, "Name");
    appendNbtString(bytes, "minecraft:diamond");
    appendNamedTag(bytes, 10, "tag");
    appendNamedTag(bytes, 8, "custom");
    appendNbtString(bytes, "preserved");
    bytes.insert(bytes.end(), 7, 0);
    return bytes;
}

void testLevelDbStructureCapture() {
    const std::string structure_name = "exchange:test_frame";
    const std::string key = "structuretemplate_" + structure_name;
    const auto nbt = frameStructureNbt();

    std::vector<std::uint8_t> batch;
    appendLittle64(batch, 123);
    appendLittle32(batch, 1);
    batch.push_back(1);
    appendVarUint(batch, static_cast<std::uint32_t>(key.size()));
    batch.insert(batch.end(), key.begin(), key.end());
    appendVarUint(batch, static_cast<std::uint32_t>(nbt.size()));
    batch.insert(batch.end(), nbt.begin(), nbt.end());

    std::vector<std::uint8_t> log(4, 0);
    appendLittle16(log, static_cast<std::uint16_t>(batch.size()));
    log.push_back(1);
    log.insert(log.end(), batch.begin(), batch.end());

    const auto directory = std::filesystem::temp_directory_path() / "endstone-exchange-leveldb-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(directory / "000001.log", std::ios::binary);
        output.write(reinterpret_cast<const char *>(log.data()), static_cast<std::streamsize>(log.size()));
    }

    const auto captured = exchange::StructureReader::readItemFrameFromLevelDbLogs(directory, structure_name);
    require(captured.has_value(), "filled item frame must be found in a LevelDB structure capture");
    require(captured->type == "minecraft:diamond" && captured->data == 0, "captured item identity");
    require(captured->nbt.contains("custom") &&
                captured->nbt.at("custom").get<endstone::StringTag>().value() == "preserved",
            "captured item NBT");
    std::filesystem::remove_all(directory, ignored);
}

void testLiveLevelDbStructureCapture() {
    const char *database_path = std::getenv("EXCHANGE_TEST_LEVELDB_PATH");
    const char *structure_name = std::getenv("EXCHANGE_TEST_STRUCTURE_NAME");
    if (database_path == nullptr || structure_name == nullptr || std::string(database_path).empty() ||
        std::string(structure_name).empty()) {
        return;
    }
    const auto captured = exchange::StructureReader::readItemFrameFromLevelDbLogs(database_path, structure_name);
    const char *expected_item = std::getenv("EXCHANGE_TEST_STRUCTURE_ITEM");
    if (expected_item == nullptr || std::string(expected_item).empty()) {
        require(!captured.has_value(), "live structure capture should contain an empty item frame");
    } else {
        require(captured.has_value() && captured->type == expected_item, "live structure capture item identity");
    }
}

void truncateExchangeTables(exchange::Database &database) {
    database.execute("SET FOREIGN_KEY_CHECKS=0");
    database.execute("TRUNCATE TABLE exchange_frame_listing_bindings");
    database.execute("TRUNCATE TABLE exchange_frame_listings");
    database.execute("TRUNCATE TABLE exchange_target_bindings");
    database.execute("TRUNCATE TABLE exchange_economy_transfers");
    database.execute("TRUNCATE TABLE exchange_balance_ledger");
    database.execute("TRUNCATE TABLE exchange_sell_escrows");
    database.execute("TRUNCATE TABLE exchange_delivery_claims");
    database.execute("TRUNCATE TABLE exchange_deliveries");
    database.execute("TRUNCATE TABLE exchange_trades");
    database.execute("TRUNCATE TABLE exchange_orders");
    database.execute("TRUNCATE TABLE exchange_markets");
    database.execute("TRUNCATE TABLE exchange_books");
    database.execute("TRUNCATE TABLE exchange_accounts");
    database.execute("SET FOREIGN_KEY_CHECKS=1");
}

void testDatabaseIntegration() {
    const char *config_path = std::getenv("EXCHANGE_TEST_DB_CONFIG");
    const char *host = std::getenv("EXCHANGE_TEST_DB_HOST");
    if ((config_path == nullptr || std::string(config_path).empty()) &&
        (host == nullptr || std::string(host).empty())) {
        std::cout << "SKIP database integration (EXCHANGE_TEST_DB_CONFIG and EXCHANGE_TEST_DB_HOST are unset)\n";
        return;
    }
    const char *user = std::getenv("EXCHANGE_TEST_DB_USER");
    const char *password = std::getenv("EXCHANGE_TEST_DB_PASSWORD");
    const char *port_text = std::getenv("EXCHANGE_TEST_DB_PORT");

    exchange::DatabaseConfig config;
    if (config_path != nullptr && !std::string(config_path).empty()) {
        config = exchange::Config::load(config_path).database;
    } else {
        config.host = host;
        config.user = user == nullptr ? "root" : user;
        config.password = password == nullptr ? "" : password;
        if (port_text != nullptr) {
            config.port = static_cast<unsigned int>(std::stoul(port_text));
        }
    }
    config.name = "endstone_exchange_test";

    exchange::Database database(config);
    database.connect();
    database.migrate();
    database.migrate();
    database.execute("DELETE FROM exchange_schema_versions WHERE version=6");
    database.execute("ALTER TABLE exchange_markets DROP COLUMN reopenable");
    database.migrate();
    const auto reopenable_column = database.query(
        "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='exchange_markets' AND COLUMN_NAME='reopenable'");
    require(reopenable_column.size() == 1, "version 5 upgrade must add the reopenable market column");
    database.migrate();
    const auto schema_version = database.query("SELECT MAX(version) FROM exchange_schema_versions");
    require(exchange::cellInt(schema_version.front(), 0) == 9, "database migrations must be idempotent at version 9");
    const auto economy_schema = database.query(
        "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='exchange_economy_transfers' AND COLUMN_NAME='amount_units'");
    require(economy_schema.size() == 1, "version 8 must create durable UMoney transfer storage");
    const auto frame_listing_schema = database.query(
        "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='exchange_frame_listings' AND COLUMN_NAME='status'");
    require(frame_listing_schema.size() == 1, "version 9 must create durable item-frame listing storage");
    const auto book_schema = database.query(
        "SELECT m.book_id,b.id FROM exchange_markets m LEFT JOIN exchange_books b ON b.id=m.book_id LIMIT 1");
    require(book_schema.empty() || exchange::cellInt64(book_schema.front(), 0) == exchange::cellInt64(book_schema.front(), 1),
            "every upgraded market must reference a durable item order book");

    truncateExchangeTables(database);
    exchange::ExchangeService upgrade_seed_service(database, 100'000, 2304);
    exchange::Market upgrade_seed;
    upgrade_seed.target_key = "block|overworld|90|64|0";
    upgrade_seed.target_kind = exchange::TargetKind::Block;
    upgrade_seed.dimension_name = "overworld";
    upgrade_seed.block_x = 90;
    upgrade_seed.block_y = 64;
    upgrade_seed.block_z = 0;
    upgrade_seed.item = {"minecraft:emerald_block", 0, {}, "Block of Emerald"};
    upgrade_seed.created_by = "upgrade-test";
    static_cast<void>(upgrade_seed_service.activateMarket(upgrade_seed));
    upgrade_seed.target_key = "block|overworld|91|64|0";
    upgrade_seed.block_x = 91;
    static_cast<void>(upgrade_seed_service.activateMarket(upgrade_seed));
    database.execute("ALTER TABLE exchange_markets DROP FOREIGN KEY fk_exchange_markets_book");
    database.execute("DROP INDEX idx_exchange_markets_book ON exchange_markets");
    database.execute("ALTER TABLE exchange_markets DROP COLUMN book_id");
    database.execute("DROP TABLE exchange_books");
    database.execute("DELETE FROM exchange_schema_versions WHERE version=7");
    database.migrate();
    const auto backfilled_books = database.query(
        "SELECT COUNT(DISTINCT book_id),(SELECT COUNT(*) FROM exchange_books) FROM exchange_markets");
    require(exchange::cellInt(backfilled_books.front(), 0) == 1 && exchange::cellInt(backfilled_books.front(), 1) == 1,
            "version 6 upgrade must merge identical item snapshots into one shared book");

    database.execute("SET SESSION wait_timeout=1");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    database.ping();
    require(!database.query("SELECT 1").empty(), "stale idle connection must reconnect outside a transaction");
    database.execute("SET SESSION wait_timeout=1");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    require(!database.query("SELECT 1").empty(), "first query after idle timeout must reconnect and retry once");
    truncateExchangeTables(database);
    exchange::ExchangeService zero_balance_service(database, 0, 640);
    require(zero_balance_service.economyBackingDeficit() == 0,
            "a fresh zero-balance economy must start fully backed");
    exchange::ExchangeService service(database, 100'000, 640);

    constexpr std::string_view Alice = "00000000-0000-0000-0000-000000000001";
    constexpr std::string_view Bob = "00000000-0000-0000-0000-000000000002";
    constexpr std::string_view Charlie = "00000000-0000-0000-0000-000000000003";
    service.ensureAccount(Alice, "Alice");
    service.ensureAccount(Bob, "Bob");
    service.ensureAccount(Charlie, "Charlie");

    const auto deposit = service.prepareEconomyTransfer(
        Alice, "Alice", exchange::EconomyTransferDirection::Deposit, 1'000, 10);
    require(deposit.status == exchange::EconomyTransferStatus::Prepared &&
                deposit.amount_units == 10 && service.balance(Alice) == 100'000,
            "deposit preparation must persist exact external units without minting exchange funds");
    service.markEconomyTransferExternalApplied(deposit.id, 990);
    service.markEconomyTransferExternalApplied(deposit.id, 990);
    require(service.completeEconomyTransfer(deposit.id) == 101'000 &&
                service.completeEconomyTransfer(deposit.id) == 101'000,
            "an externally applied deposit must credit exactly once");

    const auto withdrawal = service.prepareEconomyTransfer(
        Alice, "Alice", exchange::EconomyTransferDirection::Withdraw, 1'000, 10);
    require(service.balance(Alice) == 100'000,
            "withdrawal preparation must reserve exchange funds before touching UMoney");
    exchange::ExchangeService restarted_after_prepare(database, 100'000, 640);
    const auto prepared_after_restart = restarted_after_prepare.pendingEconomyTransfers();
    require(std::any_of(prepared_after_restart.begin(), prepared_after_restart.end(),
                        [&](const exchange::EconomyTransfer &transfer) {
                            return transfer.id == withdrawal.id &&
                                   transfer.status == exchange::EconomyTransferStatus::Prepared &&
                                   transfer.amount_units == 10;
                        }),
            "a prepared withdrawal must remain recoverable after restart");
    restarted_after_prepare.markEconomyTransferExternalApplied(withdrawal.id, 1'000);
    require(restarted_after_prepare.completeEconomyTransfer(withdrawal.id) == 100'000,
            "withdrawal completion must not debit reserved funds twice");

    const auto failed_withdrawal = service.prepareEconomyTransfer(
        Bob, "Bob", exchange::EconomyTransferDirection::Withdraw, 700, 7);
    require(service.balance(Bob) == 99'300, "failed-withdrawal test must begin with a durable reserve");
    require(service.failEconomyTransfer(failed_withdrawal.id, "not applied") == 100'000 &&
                service.failEconomyTransfer(failed_withdrawal.id, "not applied") == 100'000,
            "a definitely unapplied withdrawal must refund exactly once");

    const auto crash_deposit = service.prepareEconomyTransfer(
        Charlie, "Charlie", exchange::EconomyTransferDirection::Deposit, 500, 5);
    service.markEconomyTransferExternalApplied(crash_deposit.id, 495);
    exchange::ExchangeService restarted_after_external_apply(database, 100'000, 640);
    require(restarted_after_external_apply.completeEconomyTransfer(crash_deposit.id) == 100'500,
            "restart recovery must finish a deposit whose external side was already applied");
    const auto balancing_withdrawal = restarted_after_external_apply.prepareEconomyTransfer(
        Charlie, "Charlie", exchange::EconomyTransferDirection::Withdraw, 500, 5);
    restarted_after_external_apply.markEconomyTransferExternalApplied(balancing_withdrawal.id, 500);
    require(restarted_after_external_apply.completeEconomyTransfer(balancing_withdrawal.id) == 100'000,
            "deposit and withdrawal round trip must conserve exchange funds");

    const auto blocked_deposit = service.prepareEconomyTransfer(
        Bob, "Bob", exchange::EconomyTransferDirection::Deposit, 100, 1);
    service.blockEconomyTransfer(blocked_deposit.id, "manual reconciliation required");
    require(service.findEconomyTransfer(blocked_deposit.id)->status ==
                exchange::EconomyTransferStatus::Blocked &&
                service.pendingEconomyTransferCount() == 1,
            "ambiguous external state must be frozen for operator review rather than retried");
    bool blocked_apply_rejected = false;
    try {
        service.markEconomyTransferExternalApplied(blocked_deposit.id, 0);
    } catch (const std::exception &) {
        blocked_apply_rejected = true;
    }
    require(blocked_apply_rejected, "blocked transfers must not be auto-completed");

    endstone::CompoundTag nbt;
    nbt.insert_or_assign("custom", endstone::StringTag("preserved"));
    exchange::Market market;
    market.target_key = "block|overworld|0|64|0";
    market.target_kind = exchange::TargetKind::Block;
    market.dimension_name = "overworld";
    market.block_x = 0;
    market.block_y = 64;
    market.block_z = 0;
    market.item = {"minecraft:diamond", 0, exchange::NbtCodec::encode(nbt), "Diamond"};
    market.created_by = std::string(Alice);
    market = service.activateMarket(market);
    require(market.id != 0, "market id");

    const exchange::OrderRequest initial_sell{
        market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 100, 5};
    const auto initial_escrow = service.prepareSellEscrow(initial_sell);
    require(initial_escrow.status == exchange::SellEscrowStatus::Prepared, "sell escrow preparation");
    service.markSellEscrowTagged(initial_escrow.id, 5, 1);
    auto sell = service.executeSellEscrow(initial_escrow.id);
    require(service.executeSellEscrow(initial_escrow.id).order_id == sell.order_id,
            "executing an ordered sell escrow must be idempotent");
    require(service.unfinishedSellEscrows(Bob).front().status == exchange::SellEscrowStatus::Ordered,
            "ordered sell escrow waits for receipt cleanup");
    service.markSellEscrowCleaned(initial_escrow.id);
    require(service.unfinishedSellEscrows(Bob).empty(), "cleaned sell escrow must be final");
    const auto historical_escrow = service.findSellEscrow(initial_escrow.id, Bob);
    require(historical_escrow && historical_escrow->status == exchange::SellEscrowStatus::Ordered &&
                historical_escrow->cleaned,
            "inventory recovery must still find a cleaned sell escrow by its marker id");
    const auto canceled_escrow = service.prepareSellEscrow(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 150, 1});
    service.cancelSellEscrow(canceled_escrow.id);
    require(service.findSellEscrow(canceled_escrow.id, Bob)->status == exchange::SellEscrowStatus::Canceled,
            "inventory recovery must identify a canceled sell escrow");
    require(sell.open_quantity == 5 && sell.filled_quantity == 0, "resting sell order");
    auto book = service.orderBook(market.id, 5);
    require(book.asks.size() == 1 && book.asks.front().price_cents == 100 && book.asks.front().quantity == 5,
            "ask book aggregation");
    const auto top_books = service.topOfBooks({market.id});
    require(top_books.at(market.id).asks.size() == 1 && top_books.at(market.id).asks.front().price_cents == 100 &&
                top_books.at(market.id).asks.front().quantity == 5,
            "batched top-of-book snapshot");

    auto buy = service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Market, 0, 3});
    require(buy.filled_quantity == 3 && buy.gross_cents == 300, "market buy execution");
    require(service.balance(Alice) == 99'700, "market buyer debit");
    require(service.balance(Bob) == 100'300, "market seller credit");
    auto alice_deliveries = service.pendingDeliveries(Alice);
    require(alice_deliveries.size() == 1 && alice_deliveries.front().quantity == 3, "buyer item delivery");
    const auto prepared_claim = service.prepareDeliveryClaim(alice_deliveries.front().id, Alice, 2);
    require(prepared_claim.quantity == 2 &&
                service.unfinishedDeliveryClaims(Alice).front().status == exchange::DeliveryClaimStatus::Prepared,
            "delivery claim preparation");
    alice_deliveries = service.pendingDeliveries(Alice);
    require(alice_deliveries.front().reserved_quantity == 2, "prepared quantity must be reserved");
    service.completeDeliveryClaim(prepared_claim.id, 2);
    service.completeDeliveryClaim(prepared_claim.id, 2);
    alice_deliveries = service.pendingDeliveries(Alice);
    require(alice_deliveries.front().claimed_quantity == 2 && alice_deliveries.front().reserved_quantity == 0,
            "completed delivery claim");
    require(service.unfinishedDeliveryClaims(Alice).front().status == exchange::DeliveryClaimStatus::Applied,
            "applied delivery claim waits for marker cleanup");
    service.markDeliveryClaimCleaned(prepared_claim.id);
    require(service.unfinishedDeliveryClaims(Alice).empty(), "cleaned delivery claim must be final");
    const auto historical_claim = service.findDeliveryClaim(prepared_claim.id, Alice);
    require(historical_claim && historical_claim->status == exchange::DeliveryClaimStatus::Applied &&
                historical_claim->cleaned,
            "inventory recovery must still find a cleaned delivery claim by its marker id");
    const auto abandoned_claim = service.prepareDeliveryClaim(alice_deliveries.front().id, Alice, 1);
    service.completeDeliveryClaim(abandoned_claim.id, 0);
    require(service.findDeliveryClaim(abandoned_claim.id, Alice)->status == exchange::DeliveryClaimStatus::Canceled,
            "inventory recovery must identify a canceled delivery claim");
    require(service.pendingDeliveries(Alice).front().claimed_quantity == 2,
            "claim prepared before a crash can be safely released when no tagged item exists");

    auto bid = service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 120, 4});
    require(bid.filled_quantity == 2 && bid.open_quantity == 2, "crossing limit buy and resting remainder");
    require(service.balance(Alice) == 99'260, "limit reserve plus price-improvement refund");
    service.cancelOrder(bid.order_id, Alice);
    require(service.balance(Alice) == 99'500, "buy-order cancellation refund");

    auto no_liquidity = service.placeOrder(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Market, 0, 2});
    require(no_liquidity.filled_quantity == 0, "empty-book market sell");
    const auto bob_deliveries = service.pendingDeliveries(Bob);
    require(!bob_deliveries.empty() && bob_deliveries.back().quantity - bob_deliveries.back().claimed_quantity -
                                               bob_deliveries.back().reserved_quantity ==
                                           2,
            "unfilled market sell returned as delivery");

    static_cast<void>(service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 90, 1}));
    const auto partial_market_sell = service.placeOrder(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Market, 0, 2});
    require(partial_market_sell.filled_quantity == 1 && partial_market_sell.open_quantity == 0,
            "partially filled market sell closes immediately");
    const auto partial_market_row = database.query(
        std::format("SELECT remaining_qty,status FROM exchange_orders WHERE id={}", partial_market_sell.order_id));
    require(exchange::cellInt(partial_market_row.front(), 0) == 0 &&
                exchange::cellString(partial_market_row.front(), 1) == "PARTIAL",
            "market-order remainder must not stay open");

    book = service.orderBook(market.id, 5);
    require(book.last_price_cents == 90, "last trade price");

    const auto before_pause_alice = service.balance(Alice);
    const auto before_pause_bob = service.balance(Bob);
    const auto paused_bid = service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 80, 5});
    const auto paused_ask = service.placeOrder(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 120, 4});
    require(paused_bid.open_quantity == 5 && paused_ask.open_quantity == 4, "two-sided resting book before close");
    require(service.balance(Alice) == before_pause_alice - 400 && service.balance(Bob) == before_pause_bob,
            "resting order funding before close");

    service.closeMarket(market.id);
    service.closeMarket(market.id);
    require(!service.findMarketByTarget(market.target_key).has_value(), "closed target lookup");
    const auto paused_market = service.findMarket(market.id);
    require(paused_market.has_value() && !paused_market->active, "closed market is inactive");
    const auto paused_state = database.query(
        std::format("SELECT active,reopenable FROM exchange_markets WHERE id={}", market.id));
    require(exchange::cellInt(paused_state.front(), 0) == 0 && exchange::cellInt(paused_state.front(), 1) == 1,
            "manual close marks market reopenable");
    book = service.orderBook(market.id, 5);
    require(book.bids.size() == 1 && book.bids.front().price_cents == 80 && book.bids.front().quantity == 5 &&
                book.asks.size() == 1 && book.asks.front().price_cents == 120 && book.asks.front().quantity == 4,
            "manual close preserves the funded order book");
    require(service.balance(Alice) == before_pause_alice - 400 && service.balance(Bob) == before_pause_bob,
            "manual close does not refund or re-deliver escrow");
    const auto paused_orders = service.openOrders(Alice);
    require(std::any_of(paused_orders.begin(), paused_orders.end(),
                        [&](const exchange::OpenOrder &order) { return order.id == paused_bid.order_id; }),
            "closed-market orders remain individually cancelable");

    bool closed_order_rejected = false;
    try {
        static_cast<void>(service.placeOrder(
            {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Market, 0, 1}));
    } catch (const std::exception &) {
        closed_order_rejected = true;
    }
    require(closed_order_rejected, "closed market must reject new orders");

    auto changed_item = market;
    changed_item.id = 0;
    changed_item.item = {"minecraft:emerald", 0, {}, "Emerald"};
    changed_item.active = true;
    changed_item = service.activateMarket(changed_item);
    require(changed_item.id != market.id && service.orderBook(changed_item.id, 5).bids.empty() &&
                service.orderBook(changed_item.id, 5).asks.empty(),
            "a changed item creates an isolated market generation without inheriting old orders");
    service.closeMarket(changed_item.id);

    auto resumed = market;
    resumed.id = 0;
    resumed.active = true;
    resumed = service.activateMarket(resumed);
    require(resumed.id == market.id, "reopening the same item must resume the original market generation");
    book = service.orderBook(resumed.id, 5);
    require(book.bids.front().quantity == 5 && book.asks.front().quantity == 4,
            "resumed market exposes the preserved book");

    const auto negotiated_sell = service.placeOrder(
        {resumed.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 70, 2});
    require(negotiated_sell.filled_quantity == 2 && negotiated_sell.gross_cents == 160,
            "crossing seller accepts the resting bid price");
    const auto negotiated_buy = service.placeOrder(
        {resumed.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 130, 3});
    require(negotiated_buy.filled_quantity == 3 && negotiated_buy.gross_cents == 360,
            "crossing buyer accepts the resting ask price");
    require(service.balance(Alice) == before_pause_alice - 760 && service.balance(Bob) == before_pause_bob + 520,
            "negotiated fills settle exact maker prices");
    book = service.orderBook(resumed.id, 5);
    require(book.bids.front().quantity == 3 && book.asks.front().quantity == 1 && book.last_price_cents == 120,
            "both sides retain correct partial quantities");

    service.closeMarket(resumed.id);
    const auto balance_before_second_resume = service.balance(Alice);
    resumed = service.activateMarket(resumed);
    require(resumed.id == market.id && service.balance(Alice) == balance_before_second_resume,
            "repeat close and resume is lossless");
    service.retireMarket(resumed.id);
    require(service.openOrders(Alice).empty(), "retirement cancels the remaining buy order");
    require(service.balance(Alice) == before_pause_alice - 520,
            "retirement refunds only the unfilled buy-order reserve");
    const auto retired_state = database.query(
        std::format("SELECT active,reopenable FROM exchange_markets WHERE id={}", market.id));
    require(exchange::cellInt(retired_state.front(), 0) == 0 && exchange::cellInt(retired_state.front(), 1) == 0,
            "retired market cannot be resumed");

    auto replacement = market;
    replacement.id = 0;
    replacement.item = {"minecraft:emerald", 0, {}, "Emerald"};
    replacement.active = true;
    replacement = service.activateMarket(replacement);
    require(replacement.id != market.id, "reopening a target must create a new immutable market generation");
    require(service.findMarket(market.id)->item.type == "minecraft:diamond",
            "historical market item snapshot must never be overwritten");
    require(service.findMarketByTarget(market.target_key)->item.type == "minecraft:emerald",
            "target lookup must resolve only the current market generation");
    const auto historical_deliveries = service.pendingDeliveries(Bob);
    for (const auto &delivery : historical_deliveries) {
        if (delivery.market_id == market.id) {
            require(delivery.item.type == "minecraft:diamond",
                    "historical delivery must retain the original market item");
        }
    }

    bool duplicate_activation_rejected = false;
    try {
        static_cast<void>(service.activateMarket(replacement));
    } catch (const std::exception &) {
        duplicate_activation_rejected = true;
    }
    require(duplicate_activation_rejected, "an active target must not get a second market binding");

    const auto first_same_price_bid = service.placeOrder(
        {replacement.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 100, 1});
    const auto second_same_price_bid = service.placeOrder(
        {replacement.id, std::string(Charlie), "Charlie", exchange::Side::Buy, exchange::OrderType::Limit, 100, 1});
    static_cast<void>(first_same_price_bid);
    static_cast<void>(second_same_price_bid);
    const auto first_time_fill = service.placeOrder(
        {replacement.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Market, 0, 1});
    require(first_time_fill.filled_quantity == 1, "first same-price bid fills");
    auto latest_trade = database.query(std::format(
        "SELECT buyer_uuid FROM exchange_trades WHERE market_id={} ORDER BY id DESC LIMIT 1", replacement.id));
    require(exchange::cellString(latest_trade.front(), 0) == Alice, "same-price orders use time priority");
    const auto second_time_fill = service.placeOrder(
        {replacement.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Market, 0, 1});
    require(second_time_fill.filled_quantity == 1, "second same-price bid fills next");
    latest_trade = database.query(std::format(
        "SELECT buyer_uuid FROM exchange_trades WHERE market_id={} ORDER BY id DESC LIMIT 1", replacement.id));
    require(exchange::cellString(latest_trade.front(), 0) == Charlie, "time priority advances to the second bid");

    const auto self_ask = service.placeOrder(
        {replacement.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 150, 1});
    const auto self_buy = service.placeOrder(
        {replacement.id, std::string(Bob), "Bob", exchange::Side::Buy, exchange::OrderType::Market, 0, 1});
    require(self_buy.filled_quantity == 0, "self-trade exclusion leaves the player's own ask untouched");
    service.cancelOrder(self_ask.order_id, Bob);

    auto crossed_spread_market = replacement;
    crossed_spread_market.id = 0;
    crossed_spread_market.target_key = "block|overworld|1|64|0";
    crossed_spread_market.block_x = 1;
    crossed_spread_market = service.activateMarket(crossed_spread_market);
    const auto buyer_balance_before_cross = service.balance(Alice);
    const auto seller_balance_before_cross = service.balance(Bob);
    const auto resting_bid = service.placeOrder(
        {crossed_spread_market.id, std::string(Alice), "Alice", exchange::Side::Buy,
         exchange::OrderType::Limit, 1'000, 5});
    require(resting_bid.filled_quantity == 0 && resting_bid.open_quantity == 5 &&
                service.balance(Alice) == buyer_balance_before_cross - 5'000,
            "five-unit resting bid at 10.00 reserves exactly 50.00");

    const auto crossing_ask = service.placeOrder(
        {crossed_spread_market.id, std::string(Bob), "Bob", exchange::Side::Sell,
         exchange::OrderType::Limit, 900, 3});
    require(crossing_ask.filled_quantity == 3 && crossing_ask.open_quantity == 0 &&
                crossing_ask.gross_cents == 3'000,
            "three-unit ask at 9.00 crosses the resting 10.00 bid at the maker price");
    require(service.balance(Alice) == buyer_balance_before_cross - 5'000 &&
                service.balance(Bob) == seller_balance_before_cross + 3'000,
            "crossed-spread settlement debits the existing reserve and credits 30.00 to the seller");

    const auto crossed_orders = database.query(std::format(
        "SELECT id,price_cents,original_qty,remaining_qty,reserved_cents,status FROM exchange_orders "
        "WHERE id IN ({},{}) ORDER BY id",
        resting_bid.order_id, crossing_ask.order_id));
    require(crossed_orders.size() == 2 && exchange::cellInt64(crossed_orders[0], 1) == 1'000 &&
                exchange::cellInt(crossed_orders[0], 2) == 5 && exchange::cellInt(crossed_orders[0], 3) == 2 &&
                exchange::cellInt64(crossed_orders[0], 4) == 2'000 &&
                exchange::cellString(crossed_orders[0], 5) == "PARTIAL" &&
                exchange::cellInt64(crossed_orders[1], 1) == 900 && exchange::cellInt(crossed_orders[1], 2) == 3 &&
                exchange::cellInt(crossed_orders[1], 3) == 0 &&
                exchange::cellString(crossed_orders[1], 5) == "FILLED",
            "crossed-spread orders retain the correct prices, remaining quantity, reserve and status");
    const auto crossed_trade = database.query(std::format(
        "SELECT price_cents,quantity FROM exchange_trades WHERE buy_order_id={} AND sell_order_id={}",
        resting_bid.order_id, crossing_ask.order_id));
    require(crossed_trade.size() == 1 && exchange::cellInt64(crossed_trade.front(), 0) == 1'000 &&
                exchange::cellInt(crossed_trade.front(), 1) == 3,
            "crossed-spread trade record uses the resting bid's 10.00 maker price");
    book = service.orderBook(crossed_spread_market.id, 5);
    require(book.bids.size() == 1 && book.bids.front().price_cents == 1'000 &&
                book.bids.front().quantity == 2 && book.asks.empty() && book.last_price_cents == 1'000,
            "crossed-spread order book keeps only the remaining two-unit 10.00 bid");
    service.cancelOrder(resting_bid.order_id, Alice);
    require(service.balance(Alice) == buyer_balance_before_cross - 3'000,
            "canceling the two-unit remainder refunds exactly the remaining 20.00 reserve");

    endstone::CompoundTag shared_nbt;
    shared_nbt.insert_or_assign("series", endstone::StringTag("shared-book"));
    auto shared_market_a = replacement;
    shared_market_a.id = 0;
    shared_market_a.book_id = 0;
    shared_market_a.target_key = "block|overworld|2|64|0";
    shared_market_a.block_x = 2;
    shared_market_a.item = {"minecraft:gold_ingot", 0, exchange::NbtCodec::encode(shared_nbt), "Gold Ingot"};
    shared_market_a = service.activateMarket(shared_market_a);
    auto shared_market_b = shared_market_a;
    shared_market_b.id = 0;
    shared_market_b.book_id = 0;
    shared_market_b.target_key = "block|overworld|3|64|0";
    shared_market_b.block_x = 3;
    shared_market_b = service.activateMarket(shared_market_b);
    require(shared_market_a.id != shared_market_b.id && shared_market_a.book_id == shared_market_b.book_id,
            "same exact item at different world positions must use independent targets and one shared book");

    const auto cross_venue_bid = service.placeOrder(
        {shared_market_a.id, std::string(Charlie), "Charlie", exchange::Side::Buy,
         exchange::OrderType::Limit, 333, 4});
    book = service.orderBook(shared_market_b.id, 5);
    require(book.bids.size() == 1 && book.bids.front().price_cents == 333 && book.bids.front().quantity == 4,
            "an order placed at one position must appear at every position for the same item");
    const auto shared_tops = service.topOfBooks({shared_market_a.id, shared_market_b.id});
    require(shared_tops.at(shared_market_a.id).bids.size() == 1 &&
                shared_tops.at(shared_market_b.id).bids.size() == 1 &&
                shared_tops.at(shared_market_a.id).bids.front().price_cents ==
                    shared_tops.at(shared_market_b.id).bids.front().price_cents &&
                shared_tops.at(shared_market_a.id).bids.front().quantity ==
                    shared_tops.at(shared_market_b.id).bids.front().quantity,
            "hologram top-of-book snapshots must agree across shared-item positions");
    const auto cross_venue_sell = service.placeOrder(
        {shared_market_b.id, std::string(Bob), "Bob", exchange::Side::Sell,
         exchange::OrderType::Limit, 300, 2});
    require(cross_venue_sell.filled_quantity == 2 && cross_venue_sell.gross_cents == 666,
            "a sell submitted at another position must match the shared resting bid at maker price");
    service.closeMarket(shared_market_a.id);
    require(!service.findMarketByTarget(shared_market_a.target_key).has_value() &&
                service.findMarketByTarget(shared_market_b.target_key).has_value(),
            "closing one shared-item position must not close another position");
    book = service.orderBook(shared_market_b.id, 5);
    require(book.bids.size() == 1 && book.bids.front().quantity == 2,
            "closing one position must preserve and expose the shared remaining order");
    const auto final_cross_venue_sell = service.placeOrder(
        {shared_market_b.id, std::string(Bob), "Bob", exchange::Side::Sell,
         exchange::OrderType::Market, 0, 2});
    require(final_cross_venue_sell.filled_quantity == 2 && service.orderBook(shared_market_b.id, 5).bids.empty(),
            "the remaining shared order must stay executable through the open position");
    static_cast<void>(cross_venue_bid);
    const auto paused_market_id = shared_market_a.id;
    shared_market_a = service.activateMarket(shared_market_a);
    require(shared_market_a.id == paused_market_id && shared_market_a.book_id == shared_market_b.book_id,
            "reopening a paused position must resume its physical market on the shared book");

    const auto trades_before_concurrency = database.query(std::format(
        "SELECT COUNT(*) FROM exchange_trades t JOIN exchange_markets m ON m.id=t.market_id WHERE m.book_id={}",
        shared_market_b.book_id));
    exchange::Database concurrent_database(config);
    concurrent_database.connect();
    exchange::ExchangeService concurrent_service(concurrent_database, 100'000, 2304);
    std::barrier start_together(2);
    exchange::ExecutionResult concurrent_buy;
    exchange::ExecutionResult concurrent_sell;
    std::exception_ptr buy_error;
    std::exception_ptr sell_error;
    std::thread buyer_thread([&] {
        start_together.arrive_and_wait();
        try {
            concurrent_buy = service.placeOrder(
                {shared_market_a.id, std::string(Charlie), "Charlie", exchange::Side::Buy,
                 exchange::OrderType::Limit, 444, 1});
        } catch (...) {
            buy_error = std::current_exception();
        }
    });
    std::thread seller_thread([&] {
        start_together.arrive_and_wait();
        try {
            concurrent_sell = concurrent_service.placeOrder(
                {shared_market_b.id, std::string(Bob), "Bob", exchange::Side::Sell,
                 exchange::OrderType::Limit, 444, 1});
        } catch (...) {
            sell_error = std::current_exception();
        }
    });
    buyer_thread.join();
    seller_thread.join();
    if (buy_error) {
        std::rethrow_exception(buy_error);
    }
    if (sell_error) {
        std::rethrow_exception(sell_error);
    }
    const auto trades_after_concurrency = database.query(std::format(
        "SELECT COUNT(*) FROM exchange_trades t JOIN exchange_markets m ON m.id=t.market_id WHERE m.book_id={}",
        shared_market_b.book_id));
    require(concurrent_buy.filled_quantity + concurrent_sell.filled_quantity == 1 &&
                exchange::cellInt(trades_after_concurrency.front(), 0) ==
                    exchange::cellInt(trades_before_concurrency.front(), 0) + 1 &&
                service.orderBook(shared_market_b.id, 5).bids.empty() &&
                service.orderBook(shared_market_b.id, 5).asks.empty(),
            "simultaneous orders at different positions must serialize through the shared book without deadlock");

    auto distinct_nbt_market = shared_market_b;
    distinct_nbt_market.id = 0;
    distinct_nbt_market.book_id = 0;
    distinct_nbt_market.target_key = "block|overworld|4|64|0";
    distinct_nbt_market.block_x = 4;
    shared_nbt.insert_or_assign("series", endstone::StringTag("different-book"));
    distinct_nbt_market.item.nbt = exchange::NbtCodec::encode(shared_nbt);
    distinct_nbt_market = service.activateMarket(distinct_nbt_market);
    require(distinct_nbt_market.book_id != shared_market_b.book_id &&
                service.orderBook(distinct_nbt_market.id, 5).bids.empty(),
            "same item type with different NBT must remain an isolated product book");

    const auto excess_escrow = service.prepareSellEscrow(
        {replacement.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 250, 1});
    service.markSellEscrowTagged(excess_escrow.id, 2, 1);
    static_cast<void>(service.executeSellEscrow(excess_escrow.id));
    service.markSellEscrowCleaned(excess_escrow.id);
    const auto excess_deliveries = service.pendingDeliveries(Bob);
    require(std::any_of(excess_deliveries.begin(), excess_deliveries.end(),
                        [&](const exchange::Delivery &delivery) {
                            return delivery.market_id == replacement.id && delivery.reason == "ESCROW_EXCESS" &&
                                   delivery.quantity - delivery.claimed_quantity - delivery.reserved_quantity == 1;
                        }),
            "whole-stack sell escrow must return over-tagged items through durable delivery");

    exchange::FrameListing frame_listing;
    frame_listing.target_key = "block|overworld|40|70|40";
    frame_listing.dimension_name = "overworld";
    frame_listing.block_x = 40;
    frame_listing.block_y = 70;
    frame_listing.block_z = 40;
    frame_listing.seller_uuid = std::string(Bob);
    frame_listing.seller_name = "Bob";
    frame_listing.price_cents = 2'500;
    frame_listing.item = {"minecraft:diamond", 0, exchange::NbtCodec::encode(nbt), "Diamond"};
    const auto first_listing = service.upsertFrameListing(frame_listing);
    require(first_listing.id != 0 &&
                service.findBoundFrameListing(frame_listing.target_key)->id == first_listing.id,
            "a filled item frame must acquire one durable active listing binding");

    auto hostile_replacement = frame_listing;
    hostile_replacement.seller_uuid = std::string(Charlie);
    hostile_replacement.seller_name = "Charlie";
    bool hostile_update_rejected = false;
    try {
        static_cast<void>(service.upsertFrameListing(hostile_replacement));
    } catch (const std::exception &) {
        hostile_update_rejected = true;
    }
    require(hostile_update_rejected &&
                service.findBoundFrameListing(frame_listing.target_key)->id == first_listing.id,
            "another player must not hijack an active frame listing");

    frame_listing.price_cents = 3'000;
    const auto repriced_listing = service.upsertFrameListing(frame_listing);
    require(repriced_listing.id != first_listing.id &&
                service.findFrameListing(first_listing.id)->status == exchange::FrameListingStatus::Canceled,
            "repricing must create a new generation so an old confirmation cannot buy it");
    bool stale_confirmation_rejected = false;
    try {
        static_cast<void>(service.purchaseFrameListing(first_listing.id, Alice, "Alice", 2'500));
    } catch (const std::exception &) {
        stale_confirmation_rejected = true;
    }
    require(stale_confirmation_rejected, "a confirmation for a replaced listing must be rejected");

    bool self_purchase_rejected = false;
    try {
        static_cast<void>(service.purchaseFrameListing(repriced_listing.id, Bob, "Bob", 3'000));
    } catch (const std::exception &) {
        self_purchase_rejected = true;
    }
    require(self_purchase_rejected, "a seller must not pay themselves to remove a listing");

    const auto before_frame_buyer = service.balance(Alice);
    const auto before_frame_seller = service.balance(Bob);
    const auto purchase = service.purchaseFrameListing(repriced_listing.id, Alice, "Alice", 3'000);
    require(purchase.listing.status == exchange::FrameListingStatus::Paid &&
                purchase.buyer_balance_cents == before_frame_buyer - 3'000 &&
                purchase.seller_balance_cents == before_frame_seller + 3'000,
            "frame payment must atomically debit the buyer and credit the seller");
    bool duplicate_purchase_rejected = false;
    try {
        static_cast<void>(service.purchaseFrameListing(repriced_listing.id, Charlie, "Charlie", 3'000));
    } catch (const std::exception &) {
        duplicate_purchase_rejected = true;
    }
    require(duplicate_purchase_rejected && service.balance(Alice) == before_frame_buyer - 3'000 &&
                service.balance(Bob) == before_frame_seller + 3'000,
            "replayed or competing confirmations must not charge or credit twice");

    exchange::ExchangeService restarted_frame_service(database, 100'000, 640);
    const auto unsettled = restarted_frame_service.unsettledFrameListings();
    require(std::any_of(unsettled.begin(), unsettled.end(), [&](const exchange::FrameListing &listing) {
                return listing.id == repriced_listing.id && listing.status == exchange::FrameListingStatus::Paid;
            }),
            "a paid frame item must remain recoverable after a plugin restart");
    restarted_frame_service.markFrameListingDropped(repriced_listing.id);
    restarted_frame_service.markFrameListingDropped(repriced_listing.id);
    require(restarted_frame_service.findBoundFrameListing(frame_listing.target_key)->status ==
                exchange::FrameListingStatus::Dropped,
            "a spawned marked drop must keep the frame protected until pickup");
    restarted_frame_service.completeFrameListingPickup(repriced_listing.id);
    restarted_frame_service.completeFrameListingPickup(repriced_listing.id);
    require(!restarted_frame_service.findBoundFrameListing(frame_listing.target_key).has_value() &&
                restarted_frame_service.findFrameListing(repriced_listing.id)->status ==
                    exchange::FrameListingStatus::Claimed,
            "pickup completion must release the frame exactly once");
    const auto frame_ledger = database.query(std::format(
        "SELECT reason,delta_cents FROM exchange_balance_ledger WHERE reference_type='FRAME_LISTING' "
        "AND reference_id={} ORDER BY id",
        repriced_listing.id));
    require(frame_ledger.size() == 2 && exchange::cellString(frame_ledger.front(), 0) == "FRAME_PURCHASE" &&
                exchange::cellInt64(frame_ledger.front(), 1) == -3'000 &&
                exchange::cellString(frame_ledger.back(), 0) == "FRAME_SALE" &&
                exchange::cellInt64(frame_ledger.back(), 1) == 3'000,
            "frame transfer must leave an immutable balanced ledger pair");

    frame_listing.target_key = "block|overworld|41|70|40";
    frame_listing.block_x = 41;
    const auto canceled_listing = service.upsertFrameListing(frame_listing);
    service.cancelFrameListing(canceled_listing.id, Bob);
    require(!service.findBoundFrameListing(frame_listing.target_key).has_value() &&
                service.findFrameListing(canceled_listing.id)->status == exchange::FrameListingStatus::Canceled,
            "the seller must be able to cancel an unpaid listing without moving money");
    frame_listing.target_key = "block|overworld|42|70|40";
    frame_listing.block_x = 42;
    frame_listing.price_cents = service.balance(Charlie) + 1;
    const auto unaffordable_listing = service.upsertFrameListing(frame_listing);
    const auto before_failed_buyer = service.balance(Charlie);
    const auto before_failed_seller = service.balance(Bob);
    bool insufficient_frame_balance_rejected = false;
    try {
        static_cast<void>(service.purchaseFrameListing(unaffordable_listing.id, Charlie, "Charlie",
                                                       frame_listing.price_cents));
    } catch (const std::exception &) {
        insufficient_frame_balance_rejected = true;
    }
    require(insufficient_frame_balance_rejected && service.balance(Charlie) == before_failed_buyer &&
                service.balance(Bob) == before_failed_seller &&
                service.findBoundFrameListing(frame_listing.target_key)->status ==
                    exchange::FrameListingStatus::Active,
            "insufficient frame payment must roll back the listing and both account balances");
    service.cancelFrameListing(unaffordable_listing.id, Bob);

    bool overflow_rejected = false;
    try {
        static_cast<void>(service.addBalance(Alice, "Alice", std::numeric_limits<exchange::Cents>::max()));
    } catch (const std::exception &) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "balance overflow must be rejected");

    const auto ledger_mismatches =
        database.query("SELECT a.player_uuid FROM exchange_accounts a LEFT JOIN exchange_balance_ledger l "
                       "ON l.player_uuid=a.player_uuid GROUP BY a.player_uuid,a.balance_cents "
                       "HAVING a.balance_cents<>COALESCE(SUM(l.delta_cents),0)");
    require(ledger_mismatches.empty(), "immutable balance ledger sum must equal every account balance");
    const auto ledger_reasons = database.query("SELECT COUNT(DISTINCT reason) FROM exchange_balance_ledger");
    require(exchange::cellInt(ledger_reasons.front(), 0) >= 4,
            "balance ledger must retain distinct operational reasons");
    const auto money_total = database.query(
        "SELECT (SELECT COALESCE(SUM(balance_cents),0) FROM exchange_accounts)+"
        "(SELECT COALESCE(SUM(reserved_cents),0) FROM exchange_orders WHERE status IN ('OPEN','PARTIAL'))");
    require(exchange::cellInt64(money_total.front(), 0) == 300'000,
            "account balances plus funded buy reserves must conserve all exchange money");
    require(service.economyBackingDeficit() == 300'000,
            "legacy internal balances must be visible as unbacked before enabling UMoney");
    const auto order_invariant_failures = database.query(
        "SELECT id FROM exchange_orders WHERE "
        "(side='BUY' AND status IN ('OPEN','PARTIAL') AND reserved_cents<>price_cents*remaining_qty) OR "
        "(status IN ('FILLED','CANCELED') AND (remaining_qty<>0 OR reserved_cents<>0))");
    require(order_invariant_failures.empty(), "order quantities and buy reserves must remain internally consistent");
    const auto market_invariant_failures = database.query(
        "SELECT m.id FROM exchange_markets m LEFT JOIN exchange_target_bindings b ON b.market_id=m.id WHERE "
        "(m.active=1 AND (m.reopenable=1 OR b.market_id IS NULL)) OR "
        "(m.active=0 AND b.market_id IS NOT NULL)");
    require(market_invariant_failures.empty(), "active, reopenable and target-binding market states must agree");
}

} // namespace

int main() {
    try {
        testNbtCodec();
        testDeliveryMarkerCleanup();
        testConfig();
        testTradeFormFlow();
        testLocalization();
        testInteractionGate();
        testHologramAppearancePacket();
        testMarketDisplay();
        testCanonicalItemIdentity();
        testLevelDbStructureCapture();
        testLiveLevelDbStructureCapture();
        testDatabaseIntegration();
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
