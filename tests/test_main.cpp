#include "endstone_exchange/config.hpp"
#include "endstone_exchange/database.hpp"
#include "endstone_exchange/exchange_service.hpp"
#include "endstone_exchange/nbt_codec.hpp"
#include "endstone_exchange/structure_reader.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error("test failed: " + message);
    }
}

void testNbtCodec()
{
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
    }
    catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "truncated NBT must be rejected");
}

void testConfig()
{
    const auto path = std::filesystem::temp_directory_path() / "endstone-exchange-config-test.toml";
    exchange::Config::writeTemplate(path);
    auto config = exchange::Config::load(path);
    require(config.database.host == "127.0.0.1", "template host");
    require(config.market.initial_balance_cents == 1'000'000, "money parsing");
    require(config.market.price_step_cents == 1, "cent step parsing");
    require(config.market.frame_capture_delay_ticks == 80, "item-frame capture delay");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void appendLittle16(std::vector<std::uint8_t> &bytes, const std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLittle32(std::vector<std::uint8_t> &bytes, const std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendLittle64(std::vector<std::uint8_t> &bytes, const std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendVarUint(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        bytes.push_back(byte);
    } while (value != 0);
}

void appendNbtString(std::vector<std::uint8_t> &bytes, const std::string_view value)
{
    appendLittle16(bytes, static_cast<std::uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendNamedTag(std::vector<std::uint8_t> &bytes, const std::uint8_t type, const std::string_view name)
{
    bytes.push_back(type);
    appendNbtString(bytes, name);
}

std::vector<std::uint8_t> frameStructureNbt()
{
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

void testLevelDbStructureCapture()
{
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

void testLiveLevelDbStructureCapture()
{
    const char *database_path = std::getenv("EXCHANGE_TEST_LEVELDB_PATH");
    const char *structure_name = std::getenv("EXCHANGE_TEST_STRUCTURE_NAME");
    if (database_path == nullptr || structure_name == nullptr || std::string(database_path).empty() ||
        std::string(structure_name).empty()) {
        return;
    }
    const auto captured =
        exchange::StructureReader::readItemFrameFromLevelDbLogs(database_path, structure_name);
    const char *expected_item = std::getenv("EXCHANGE_TEST_STRUCTURE_ITEM");
    if (expected_item == nullptr || std::string(expected_item).empty()) {
        require(!captured.has_value(), "live structure capture should contain an empty item frame");
    }
    else {
        require(captured.has_value() && captured->type == expected_item, "live structure capture item identity");
    }
}

void truncateExchangeTables(exchange::Database &database)
{
    database.execute("SET FOREIGN_KEY_CHECKS=0");
    database.execute("TRUNCATE TABLE exchange_deliveries");
    database.execute("TRUNCATE TABLE exchange_trades");
    database.execute("TRUNCATE TABLE exchange_orders");
    database.execute("TRUNCATE TABLE exchange_markets");
    database.execute("TRUNCATE TABLE exchange_accounts");
    database.execute("SET FOREIGN_KEY_CHECKS=1");
}

void testDatabaseIntegration()
{
    const char *host = std::getenv("EXCHANGE_TEST_DB_HOST");
    if (host == nullptr || std::string(host).empty()) {
        std::cout << "SKIP database integration (EXCHANGE_TEST_DB_HOST is unset)\n";
        return;
    }
    const char *user = std::getenv("EXCHANGE_TEST_DB_USER");
    const char *password = std::getenv("EXCHANGE_TEST_DB_PASSWORD");
    const char *port_text = std::getenv("EXCHANGE_TEST_DB_PORT");

    exchange::DatabaseConfig config;
    config.host = host;
    config.user = user == nullptr ? "root" : user;
    config.password = password == nullptr ? "" : password;
    config.name = "endstone_exchange_test";
    if (port_text != nullptr) {
        config.port = static_cast<unsigned int>(std::stoul(port_text));
    }

    exchange::Database database(config);
    database.connect();
    database.migrate();
    truncateExchangeTables(database);
    exchange::ExchangeService service(database, 100'000, 2304);

    constexpr std::string_view Alice = "00000000-0000-0000-0000-000000000001";
    constexpr std::string_view Bob = "00000000-0000-0000-0000-000000000002";
    service.ensureAccount(Alice, "Alice");
    service.ensureAccount(Bob, "Bob");

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

    auto sell = service.placeOrder(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Limit, 100, 5});
    require(sell.open_quantity == 5 && sell.filled_quantity == 0, "resting sell order");
    auto book = service.orderBook(market.id, 5);
    require(book.asks.size() == 1 && book.asks.front().price_cents == 100 && book.asks.front().quantity == 5,
            "ask book aggregation");

    auto buy = service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Market, 0, 3});
    require(buy.filled_quantity == 3 && buy.gross_cents == 300, "market buy execution");
    require(service.balance(Alice) == 99'700, "market buyer debit");
    require(service.balance(Bob) == 100'300, "market seller credit");
    auto alice_deliveries = service.pendingDeliveries(Alice);
    require(alice_deliveries.size() == 1 && alice_deliveries.front().quantity == 3,
            "buyer item delivery");
    service.markDeliveryClaimed(alice_deliveries.front().id, 2);
    alice_deliveries = service.pendingDeliveries(Alice);
    require(alice_deliveries.front().claimed_quantity == 2, "partial delivery claim");

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
    require(!bob_deliveries.empty() && bob_deliveries.back().quantity - bob_deliveries.back().claimed_quantity == 2,
            "unfilled market sell returned as delivery");

    static_cast<void>(service.placeOrder(
        {market.id, std::string(Alice), "Alice", exchange::Side::Buy, exchange::OrderType::Limit, 90, 1}));
    const auto partial_market_sell = service.placeOrder(
        {market.id, std::string(Bob), "Bob", exchange::Side::Sell, exchange::OrderType::Market, 0, 2});
    require(partial_market_sell.filled_quantity == 1 && partial_market_sell.open_quantity == 0,
            "partially filled market sell closes immediately");
    const auto partial_market_row = database.query(std::format(
        "SELECT remaining_qty,status FROM exchange_orders WHERE id={}", partial_market_sell.order_id));
    require(exchange::cellInt(partial_market_row.front(), 0) == 0 &&
                exchange::cellString(partial_market_row.front(), 1) == "PARTIAL",
            "market-order remainder must not stay open");

    book = service.orderBook(market.id, 5);
    require(book.last_price_cents == 90, "last trade price");
    service.deactivateMarket(market.id);
    require(!service.findMarketByTarget(market.target_key).has_value(), "deactivated target lookup");

    bool overflow_rejected = false;
    try {
        static_cast<void>(service.addBalance(Alice, "Alice", std::numeric_limits<exchange::Cents>::max()));
    }
    catch (const std::exception &) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "balance overflow must be rejected");
}

}  // namespace

int main()
{
    try {
        testNbtCodec();
        testConfig();
        testLevelDbStructureCapture();
        testLiveLevelDbStructureCapture();
        testDatabaseIntegration();
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
