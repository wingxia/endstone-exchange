#include "endstone_exchange/plugin/ExchangePlugin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>

#include <endstone/color_format.h>
#include <endstone/event/player/player_quit_event.h>
#include <endstone/event/server/packet_receive_event.h>
#include <endstone/event/server/packet_send_event.h>
#include <endstone/form/action_form.h>
#include <endstone/form/controls/text_input.h>
#include <endstone/form/modal_form.h>
#include <endstone/inventory/item_stack.h>

#include "endstone_exchange/catalog/GeneratedCatalog.hpp"
namespace exchange::plugin {
namespace {

constexpr std::size_t kDashboardPageSize = ui::chest_layout::kProductSlots;
constexpr std::uint8_t kChestContainerId = 200;
constexpr std::uint8_t kChestContainerType = 0x0;
constexpr std::uint8_t kLevelEntityContainer = 7;
constexpr int kPacketUpdateBlock = 21;
constexpr int kPacketContainerOpen = 46;
constexpr int kPacketContainerClose = 47;
constexpr int kPacketInventoryContent = 49;
constexpr int kPacketBlockActorData = 56;
constexpr int kPacketPing = 115;
constexpr int kPacketItemStackRequest = 147;
constexpr int kPacketPacketViolationWarning = 156;
constexpr int kPacketItemRegistry = 162;

struct BridgeConfig {
    std::string host{"127.0.0.1"};
    int port{8765};
    std::string token;
};

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::optional<BridgeConfig> loadBridgeConfig() {
    const auto path = std::filesystem::current_path() / "plugins" / "exchange_umoney_bridge" / "config.yml";
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    BridgeConfig config;
    std::string line;
    while (std::getline(file, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, pos));
        auto value = trim(line.substr(pos + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "host") {
            config.host = value;
        } else if (key == "port") {
            config.port = std::stoi(value);
        } else if (key == "token") {
            config.token = value;
        }
    }
    if (config.token.empty()) {
        return std::nullopt;
    }
    return config;
}

bool looksLikeProductKey(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : value) {
        if (c == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::vector<std::string> parseModalValues(const std::string& json) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto skip = [&]() {
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
    };
    skip();
    if (i >= json.size() || json[i] != '[') {
        return out;
    }
    ++i;
    while (i < json.size()) {
        skip();
        if (i >= json.size() || json[i] == ']') {
            break;
        }
        if (json[i] == '"') {
            ++i;
            std::string value;
            while (i < json.size()) {
                const char c = json[i++];
                if (c == '"') {
                    break;
                }
                if (c == '\\' && i < json.size()) {
                    value.push_back(json[i++]);
                } else {
                    value.push_back(c);
                }
            }
            out.push_back(value);
        } else {
            const auto start = i;
            while (i < json.size() && json[i] != ',' && json[i] != ']') {
                ++i;
            }
            out.push_back(trim(json.substr(start, i - start)));
        }
        skip();
        if (i < json.size() && json[i] == ',') {
            ++i;
        }
    }
    return out;
}

std::optional<std::int32_t> parsePositiveInt32(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed <= 0 || parsed > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(parsed);
}

std::optional<std::int64_t> parsePositiveInt64(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed <= 0) {
        return std::nullopt;
    }
    return parsed;
}

std::string actionToken(ui::ActionKind action) {
    switch (action) {
    case ui::ActionKind::MarketBuy:
        return "market_buy";
    case ui::ActionKind::LimitBuy:
        return "limit_buy";
    case ui::ActionKind::MarketSell:
        return "market_sell";
    case ui::ActionKind::LimitSell:
        return "limit_sell";
    default:
        return "unknown";
    }
}

std::optional<ui::ActionKind> actionFromToken(const std::string& token) {
    if (token == "market_buy") {
        return ui::ActionKind::MarketBuy;
    }
    if (token == "limit_buy") {
        return ui::ActionKind::LimitBuy;
    }
    if (token == "market_sell") {
        return ui::ActionKind::MarketSell;
    }
    if (token == "limit_sell") {
        return ui::ActionKind::LimitSell;
    }
    return std::nullopt;
}

std::string tradePayload(ui::ActionKind action, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price) {
    return actionToken(action) + "|" + product_key + "|" + std::to_string(quantity) + "|" + std::to_string(unit_price);
}

std::pair<std::string, std::size_t> parsePageTarget(const std::string& target) {
    const auto parts = split(target, '|');
    if (parts.size() < 2) {
        return {target, 0};
    }
    auto page = parsePositiveInt64(parts[1]);
    return {parts[0], page ? static_cast<std::size_t>(*page) : 0};
}

std::optional<std::size_t> parsePageIndex(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

struct SlotItem {
    std::string item_id{"minecraft:air"};
    std::string name;
    std::vector<std::string> lore;
    int amount{1};
    bool glint{false};
};

struct ChestRender {
    std::array<SlotItem, ui::chest_layout::kTotalSlots> items;
    std::unordered_map<int, ChestSlotAction> actions;
};

class BinaryStream {
public:
    [[nodiscard]] const std::string& getBuffer() const { return buffer_; }

    void writeRawBytes(std::string_view value) { buffer_.append(value.data(), value.size()); }

    void writeBool(bool value, const char*, const char*) { writeByte(value ? 1 : 0, nullptr, nullptr); }

    void writeByte(std::uint8_t value, const char*, const char*) {
        buffer_.push_back(static_cast<char>(value));
    }

    void writeUnsignedShort(std::uint16_t value, const char*, const char*) {
        writeLittle(value);
    }

    void writeSignedShort(std::int16_t value, const char*, const char*) {
        writeLittle(static_cast<std::uint16_t>(value));
    }

    void writeUnsignedInt(std::uint32_t value, const char*, const char*) {
        writeLittle(value);
    }

    void writeSignedInt(std::int32_t value, const char*, const char*) {
        writeLittle(static_cast<std::uint32_t>(value));
    }

    void writeUnsignedInt64(std::uint64_t value, const char*, const char*) {
        writeLittle(value);
    }

    void writeUnsignedVarInt(std::uint32_t value, const char*, const char*) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 0x7f);
            value >>= 7U;
            if (value != 0) {
                byte |= 0x80;
            }
            writeByte(byte, nullptr, nullptr);
        } while (value != 0);
    }

    void writeUnsignedVarInt64(std::uint64_t value, const char*, const char*) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 0x7f);
            value >>= 7U;
            if (value != 0) {
                byte |= 0x80;
            }
            writeByte(byte, nullptr, nullptr);
        } while (value != 0);
    }

    void writeVarInt(std::int32_t value, const char*, const char*) {
        const auto raw = static_cast<std::uint32_t>(value);
        const auto zigzag = (raw << 1U) ^ static_cast<std::uint32_t>(value >> 31);
        writeUnsignedVarInt(zigzag, nullptr, nullptr);
    }

    void writeVarInt64(std::int64_t value, const char*, const char*) {
        const auto raw = static_cast<std::uint64_t>(value);
        const auto zigzag = (raw << 1U) ^ static_cast<std::uint64_t>(value >> 63);
        writeUnsignedVarInt64(zigzag, nullptr, nullptr);
    }

    void writeString(std::string_view value, const char*, const char*) {
        writeUnsignedVarInt(static_cast<std::uint32_t>(value.size()), nullptr, nullptr);
        writeRawBytes(value);
    }

private:
    template <typename T>
    void writeLittle(T value) {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            buffer_.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }

    std::string buffer_;
};

std::string money(std::optional<std::int64_t> value) {
    return value ? std::to_string(*value) : "-";
}

std::string spread(const Quote& quote) {
    if (!quote.best_bid || !quote.best_ask) {
        return "-";
    }
    return std::to_string(*quote.best_ask - *quote.best_bid);
}

std::string compactText(const std::string& value, std::size_t max_chars) {
    if (value.size() <= max_chars) {
        return value;
    }
    if (max_chars <= 3) {
        return value.substr(0, max_chars);
    }
    return value.substr(0, max_chars - 3) + "...";
}

std::string categoryIconItem(const ui::CategorySpec& category) {
    if (category.icon == "textures/items/book_normal") {
        return "minecraft:book";
    }
    if (category.icon == "textures/items/minecart_chest") {
        return "minecraft:chest_minecart";
    }
    const auto slash = category.icon.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= category.icon.size()) {
        return "minecraft:paper";
    }
    return "minecraft:" + category.icon.substr(slash + 1);
}

void writeBlockPos(BinaryStream& stream, int x, int y, int z) {
    stream.writeVarInt(x, "x", nullptr);
    stream.writeVarInt(y, "y", nullptr);
    stream.writeVarInt(z, "z", nullptr);
}

std::string packetPayload(BinaryStream& stream) {
    return stream.getBuffer();
}

std::string updateBlockPayload(int x, int y, int z, std::uint32_t runtime_id) {
    BinaryStream stream;
    writeBlockPos(stream, x, y, z);
    stream.writeUnsignedVarInt(runtime_id, "block runtime id", nullptr);
    stream.writeUnsignedVarInt(0b0010, "flags", nullptr);
    stream.writeUnsignedVarInt(0, "layer", nullptr);
    return packetPayload(stream);
}

void writeNbtStringRaw(std::string& out, std::string_view value) {
    const auto size = static_cast<std::uint16_t>(value.size());
    out.push_back(static_cast<char>(size & 0xff));
    out.push_back(static_cast<char>((size >> 8) & 0xff));
    out.append(value.data(), value.size());
}

void writeNbtNamedHeader(std::string& out, std::uint8_t type, std::string_view name) {
    out.push_back(static_cast<char>(type));
    writeNbtStringRaw(out, name);
}

void writeNbtString(std::string& out, std::string_view name, std::string_view value) {
    writeNbtNamedHeader(out, 8, name);
    writeNbtStringRaw(out, value);
}

void writeNbtInt(std::string& out, std::string_view name, std::int32_t value) {
    writeNbtNamedHeader(out, 3, name);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        out.push_back(static_cast<char>((static_cast<std::uint32_t>(value) >> (i * 8)) & 0xff));
    }
}

std::string displayNbt(const SlotItem& slot) {
    if (slot.name.empty() && slot.lore.empty()) {
        return {};
    }
    std::string out;
    writeNbtNamedHeader(out, 10, "display");
    if (!slot.name.empty()) {
        writeNbtString(out, "Name", slot.name);
    }
    if (!slot.lore.empty()) {
        writeNbtNamedHeader(out, 9, "Lore");
        out.push_back(static_cast<char>(8));
        const auto count = static_cast<std::uint32_t>(slot.lore.size());
        for (std::size_t i = 0; i < sizeof(count); ++i) {
            out.push_back(static_cast<char>((count >> (i * 8)) & 0xff));
        }
        for (const auto& line : slot.lore) {
            writeNbtStringRaw(out, line);
        }
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::string blockActorPayloadManual(int x, int y, int z, const std::string& title) {
    std::string nbt;
    writeNbtString(nbt, "id", "Chest");
    writeNbtString(nbt, "CustomName", title);
    writeNbtInt(nbt, "x", x);
    writeNbtInt(nbt, "y", y);
    writeNbtInt(nbt, "z", z);
    nbt.push_back(0);
    BinaryStream stream;
    writeBlockPos(stream, x, y, z);
    stream.writeRawBytes(nbt);
    return packetPayload(stream);
}

std::string containerOpenPayload(int x, int y, int z) {
    BinaryStream stream;
    stream.writeByte(kChestContainerId, "container id", nullptr);
    stream.writeByte(kChestContainerType, "container type", nullptr);
    writeBlockPos(stream, x, y, z);
    stream.writeVarInt64(-1, "target actor id", nullptr);
    return packetPayload(stream);
}

std::string containerClosePayload() {
    BinaryStream stream;
    stream.writeByte(kChestContainerId, "container id", nullptr);
    stream.writeByte(kChestContainerType, "container type", nullptr);
    stream.writeBool(true, "server side", nullptr);
    return packetPayload(stream);
}

std::string pingPayload(std::int64_t timestamp) {
    BinaryStream stream;
    stream.writeUnsignedInt64(static_cast<std::uint64_t>(timestamp), "timestamp", nullptr);
    stream.writeBool(true, "from server", nullptr);
    return packetPayload(stream);
}

std::string normalizedItemId(std::string item_id) {
    if (item_id.empty()) {
        return "minecraft:air";
    }
    if (item_id.find(':') == std::string::npos) {
        return "minecraft:" + item_id;
    }
    return item_id;
}

std::int16_t runtimeIdFor(const std::unordered_map<std::string, std::int16_t>& runtime_ids, const SlotItem& slot) {
    auto item_id = normalizedItemId(slot.item_id);
    auto it = runtime_ids.find(item_id);
    if (it != runtime_ids.end()) {
        return it->second;
    }
    it = runtime_ids.find("minecraft:barrier");
    if (it != runtime_ids.end()) {
        return it->second;
    }
    return 0;
}

void writeItemUserData(BinaryStream& stream, const SlotItem& slot) {
    BinaryStream user_data;
    const auto nbt = displayNbt(slot);
    if (!nbt.empty()) {
        user_data.writeSignedShort(-1, "nbt marker", nullptr);
        user_data.writeByte(1, "nbt version", nullptr);
        user_data.writeRawBytes(nbt);
    } else {
        user_data.writeSignedShort(0, "nbt marker", nullptr);
    }
    user_data.writeUnsignedInt(0, "can place on count", nullptr);
    user_data.writeUnsignedInt(0, "can destroy count", nullptr);
    stream.writeString(user_data.getBuffer(), "user data", nullptr);
}

void writeNetworkItem(BinaryStream& stream, const SlotItem& slot, const std::unordered_map<std::string, std::int16_t>& runtime_ids) {
    const auto item_id = normalizedItemId(slot.item_id);
    if (item_id == "minecraft:air") {
        stream.writeVarInt(0, "id", nullptr);
        return;
    }
    const auto runtime_id = runtimeIdFor(runtime_ids, slot);
    if (runtime_id == 0) {
        stream.writeVarInt(0, "id", nullptr);
        return;
    }
    stream.writeVarInt(runtime_id, "id", nullptr);
    stream.writeUnsignedShort(static_cast<std::uint16_t>(std::max(1, std::min(slot.amount, 64))), "stack size", nullptr);
    stream.writeUnsignedVarInt(0, "aux", nullptr);
    stream.writeBool(false, "has net id", nullptr);
    stream.writeVarInt(0, "block runtime id", nullptr);
    writeItemUserData(stream, slot);
}

std::string inventoryContentPayload(const std::array<SlotItem, ui::chest_layout::kTotalSlots>& items,
                                    const std::unordered_map<std::string, std::int16_t>& runtime_ids) {
    BinaryStream stream;
    stream.writeUnsignedVarInt(kChestContainerId, "container id", nullptr);
    stream.writeUnsignedVarInt(static_cast<std::uint32_t>(items.size()), "item count", nullptr);
    for (const auto& item : items) {
        writeNetworkItem(stream, item, runtime_ids);
    }
    stream.writeByte(kLevelEntityContainer, "container enum", nullptr);
    stream.writeBool(false, "dynamic slot", nullptr);
    writeNetworkItem(stream, SlotItem{}, runtime_ids);
    return packetPayload(stream);
}

class PacketReader {
public:
    explicit PacketReader(std::string_view payload) : payload_(payload) {}

    std::optional<std::uint8_t> byte() {
        if (pos_ >= payload_.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(payload_[pos_++]);
    }

    std::optional<std::uint32_t> unsignedVarInt() {
        std::uint32_t value = 0;
        for (int shift = 0; shift <= 28; shift += 7) {
            const auto b = byte();
            if (!b) {
                return std::nullopt;
            }
            value |= static_cast<std::uint32_t>(*b & 0x7f) << shift;
            if ((*b & 0x80U) == 0) {
                return value;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint16_t> unsignedShort() {
        if (pos_ + sizeof(std::uint16_t) > payload_.size()) {
            return std::nullopt;
        }
        std::uint16_t value = static_cast<std::uint8_t>(payload_[pos_]) |
                              (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload_[pos_ + 1])) << 8U);
        pos_ += sizeof(std::uint16_t);
        return value;
    }

    std::optional<std::int16_t> signedShort() {
        const auto value = unsignedShort();
        if (!value) {
            return std::nullopt;
        }
        return static_cast<std::int16_t>(*value);
    }

    std::optional<std::int32_t> varInt() {
        const auto raw = unsignedVarInt();
        if (!raw) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>((*raw >> 1U) ^ (~(*raw & 1U) + 1U));
    }

    std::optional<std::string> string() {
        const auto length = unsignedVarInt();
        if (!length || pos_ + *length > payload_.size()) {
            return std::nullopt;
        }
        std::string value(payload_.substr(pos_, *length));
        pos_ += *length;
        return value;
    }

    std::optional<std::string> nbtString() {
        const auto length = unsignedShort();
        if (!length || pos_ + *length > payload_.size()) {
            return std::nullopt;
        }
        std::string value(payload_.substr(pos_, *length));
        pos_ += *length;
        return value;
    }

    bool skipUnsignedInt() {
        if (pos_ + sizeof(std::uint32_t) > payload_.size()) {
            return false;
        }
        pos_ += sizeof(std::uint32_t);
        return true;
    }

    bool skipBytes(std::size_t count) {
        if (pos_ + count > payload_.size()) {
            return false;
        }
        pos_ += count;
        return true;
    }

    bool skipNbtPayload(std::uint8_t type) {
        switch (type) {
        case 0:
            return true;
        case 1:
            return skipBytes(1);
        case 2:
            return skipBytes(2);
        case 3:
        case 5:
            return skipBytes(4);
        case 4:
        case 6:
            return skipBytes(8);
        case 7: {
            const auto count = littleInt32();
            return count && *count >= 0 && skipBytes(static_cast<std::size_t>(*count));
        }
        case 8:
            return nbtString().has_value();
        case 9: {
            const auto child_type = byte();
            const auto count = littleInt32();
            if (!child_type || !count || *count < 0) {
                return false;
            }
            for (std::int32_t i = 0; i < *count; ++i) {
                if (!skipNbtPayload(*child_type)) {
                    return false;
                }
            }
            return true;
        }
        case 10:
            return skipNbtCompound();
        case 11: {
            const auto count = littleInt32();
            return count && *count >= 0 && skipBytes(static_cast<std::size_t>(*count) * 4);
        }
        case 12: {
            const auto count = littleInt32();
            return count && *count >= 0 && skipBytes(static_cast<std::size_t>(*count) * 8);
        }
        default:
            return false;
        }
    }

    bool skipNbtCompound() {
        while (true) {
            const auto type = byte();
            if (!type) {
                return false;
            }
            if (*type == 0) {
                return true;
            }
            if (!nbtString()) {
                return false;
            }
            if (!skipNbtPayload(*type)) {
                return false;
            }
        }
    }

private:
    std::optional<std::int32_t> littleInt32() {
        if (pos_ + sizeof(std::int32_t) > payload_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload_[pos_ + i])) << (i * 8);
        }
        pos_ += sizeof(value);
        return static_cast<std::int32_t>(value);
    }

    std::string_view payload_;
    std::size_t pos_{0};
};

std::optional<int> readRequestSlot(PacketReader& reader) {
    const auto container = reader.byte();
    if (!container) {
        return std::nullopt;
    }
    const auto has_dynamic = reader.byte();
    if (!has_dynamic) {
        return std::nullopt;
    }
    if (*has_dynamic != 0 && !reader.skipUnsignedInt()) {
        return std::nullopt;
    }
    const auto slot = reader.byte();
    if (!slot) {
        return std::nullopt;
    }
    if (!reader.varInt()) {
        return std::nullopt;
    }
    if (*container != kLevelEntityContainer) {
        return std::nullopt;
    }
    return static_cast<int>(*slot);
}

std::optional<int> clickedSlotFromItemStackRequest(std::string_view payload) {
    PacketReader reader(payload);
    const auto request_count = reader.unsignedVarInt();
    if (!request_count) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < *request_count; ++i) {
        if (!reader.varInt()) {
            return std::nullopt;
        }
        const auto action_count = reader.unsignedVarInt();
        if (!action_count) {
            return std::nullopt;
        }
        for (std::uint32_t a = 0; a < *action_count; ++a) {
            const auto action_type = reader.byte();
            if (!action_type) {
                return std::nullopt;
            }
            if (*action_type == 0 || *action_type == 1) {
                if (!reader.byte()) {
                    return std::nullopt;
                }
                auto source_slot = readRequestSlot(reader);
                auto destination_slot = readRequestSlot(reader);
                if (source_slot) {
                    return source_slot;
                }
                if (destination_slot) {
                    return destination_slot;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::int64_t randomAckTimestamp() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::int64_t> dist(1, 32767);
    return dist(rng);
}

std::optional<std::int64_t> pingTimestampFromPayload(std::string_view payload) {
    if (payload.size() < sizeof(std::uint64_t)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    std::memcpy(&value, payload.data(), sizeof(value));
    return static_cast<std::int64_t>(value);
}

std::unordered_map<std::string, std::int16_t> itemRuntimeIdsFromRegistry(std::string_view payload) {
    std::unordered_map<std::string, std::int16_t> out;
    PacketReader reader(payload);
    const auto count = reader.unsignedVarInt();
    if (!count) {
        return out;
    }
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto name = reader.string();
        auto id = reader.signedShort();
        auto component_based = reader.byte();
        auto version = reader.varInt();
        if (!name || !id || !component_based || !version) {
            break;
        }
        out.emplace(*name, *id);
        if (!reader.skipNbtCompound()) {
            break;
        }
    }
    return out;
}

}  // namespace

void ExchangePlugin::onEnable() {
    repository_ = std::make_unique<InMemoryRepository>();
    if (auto bridge = loadBridgeConfig()) {
        economy_ = std::make_unique<HttpBridgeEconomy>(bridge->host, bridge->port, bridge->token);
        getLogger().info("Exchange economy connected to UMoney bridge at " + bridge->host + ":" + std::to_string(bridge->port));
    } else {
        economy_ = std::make_unique<FakeEconomy>();
        getLogger().warning("Exchange UMoney bridge config was not found; using in-memory test economy");
    }
    service_ = std::make_unique<ExchangeService>(*repository_, *economy_);
    seedCatalog();
    registerEvent(&ExchangePlugin::handlePacketReceive, *this);
    registerEvent(&ExchangePlugin::handlePacketSend, *this);
    registerEvent(&ExchangePlugin::handlePlayerQuit, *this);
    getLogger().info("Endstone Exchange enabled");
}

void ExchangePlugin::onDisable() {
    getLogger().info("Endstone Exchange disabled");
}

bool ExchangePlugin::onCommand(endstone::CommandSender& sender, const endstone::Command& command, const std::vector<std::string>& args) {
    const auto name = command.getName();
    if (name != "ex" && name != "exchange") {
        return false;
    }
    if (!args.empty() && args.front() == "reload") {
        seedCatalog();
        sender.sendMessage("Exchange catalog reloaded.");
        return true;
    }
    auto* player = sender.asPlayer();
    if (player == nullptr) {
        sender.sendErrorMessage("Exchange UI can only be opened by players.");
        return true;
    }
    if (!args.empty() && args.front() == "admin") {
        openAdmin(*player);
        return true;
    }
    openHome(*player);
    return true;
}

void ExchangePlugin::seedCatalog() {
    for (const auto& product : catalog::products()) {
        repository_->upsertProduct(product);
    }
    getLogger().info("Exchange catalog loaded " + std::to_string(catalog::products().size()) + " products from generated Minecraft Wiki data");
}

void ExchangePlugin::openHome(endstone::Player& player) {
    openChestDashboard(player);
}

void ExchangePlugin::openChestDashboard(endstone::Player& player) {
    if (item_runtime_ids_.empty() || item_runtime_ids_.count("minecraft:barrier") == 0) {
        sendNotice(player, "物品图标表还未加载完成，请重新进服或稍后再执行 /ex。");
        return;
    }
    auto& session = chest_sessions_[player.getUniqueId().str()];
    session.dashboard = dashboardState(player);

    const auto loc = player.getLocation();
    constexpr auto kPi = 3.14159265358979323846;
    const auto yaw = (loc.getYaw() + 180.0F) * static_cast<float>(kPi / 180.0);
    session.block_x = static_cast<int>(std::floor(loc.getX() - std::sin(yaw) * 2.0F));
    session.block_y = loc.getBlockY() + 1;
    session.block_z = static_cast<int>(std::floor(loc.getZ() + std::cos(yaw) * 2.0F));
    session.second_block_x = (session.block_x & 1) != 0 ? session.block_x + 1 : session.block_x - 1;
    sendChestGraphic(player, session);
}

void ExchangePlugin::openChestProduct(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openChestDashboard(player);
        return;
    }
    auto& state = dashboardState(player);
    state.search_query.clear();
    search_queries_.erase(player.getUniqueId().str());
    state.category_id = product->category;
    state.product_key = product_key;
    const auto products = service_->getCatalog(state.category_id);
    auto it = std::find_if(products.begin(), products.end(), [&](const Product& candidate) {
        return candidate.product_key == product_key;
    });
    if (it != products.end()) {
        const auto index = static_cast<std::size_t>(std::distance(products.begin(), it));
        state.page = index / kDashboardPageSize;
    }
    refreshChest(player);
}

void ExchangePlugin::refreshChest(endstone::Player& player) {
    auto& session = chest_sessions_[player.getUniqueId().str()];
    if (session.state == ChestSessionState::Open) {
        session.dashboard = dashboardState(player);
        sendChestContents(player, session);
        return;
    }
    openChestDashboard(player);
}

void ExchangePlugin::closeChest(endstone::Player& player, bool remove_blocks) {
    const auto key = player.getUniqueId().str();
    auto it = chest_sessions_.find(key);
    if (it == chest_sessions_.end()) {
        return;
    }
    auto session = it->second;
    it->second.state = ChestSessionState::Closing;
    try {
        player.sendPacket(kPacketContainerClose, containerClosePayload());
    } catch (...) {
    }
    if (remove_blocks) {
        try {
            auto first = player.getDimension().getBlockAt(session.block_x, session.block_y, session.block_z);
            if (first) {
                player.sendPacket(kPacketUpdateBlock,
                                  updateBlockPayload(session.block_x, session.block_y, session.block_z,
                                                     first->getData()->getRuntimeId()));
            }
            auto second = player.getDimension().getBlockAt(session.second_block_x, session.block_y, session.block_z);
            if (second) {
                player.sendPacket(kPacketUpdateBlock,
                                  updateBlockPayload(session.second_block_x, session.block_y, session.block_z,
                                                     second->getData()->getRuntimeId()));
            }
        } catch (...) {
        }
    }
    chest_sessions_.erase(key);
}

void ExchangePlugin::sendChestGraphic(endstone::Player& player, ChestSession& session) {
    const auto chest = getServer().createBlockData("minecraft:chest");
    if (!chest) {
        sendNotice(player, "无法创建箱子 UI 方块。");
        return;
    }
    const auto runtime_id = chest->getRuntimeId();
    player.sendPacket(kPacketUpdateBlock,
                      updateBlockPayload(session.block_x, session.block_y, session.block_z, runtime_id));
    player.sendPacket(kPacketUpdateBlock,
                      updateBlockPayload(session.second_block_x, session.block_y, session.block_z, runtime_id));
    session.state = ChestSessionState::GraphicSent;
    const auto ack = randomAckTimestamp();
    session.ack_timestamp = ack * 1000000;
    player.sendPacket(kPacketPing, pingPayload(ack));
}

void ExchangePlugin::sendChestGraphicData(endstone::Player& player, ChestSession& session) {
    player.sendPacket(kPacketBlockActorData,
                      blockActorPayloadManual(session.block_x, session.block_y, session.block_z, "Exchange"));
    player.sendPacket(kPacketBlockActorData,
                      blockActorPayloadManual(session.second_block_x, session.block_y, session.block_z, "Exchange"));
    session.state = ChestSessionState::GraphicDataSent;
    const auto ack = randomAckTimestamp();
    session.ack_timestamp = ack * 1000000;
    player.sendPacket(kPacketPing, pingPayload(ack));
}

void ExchangePlugin::sendChestOpen(endstone::Player& player, ChestSession& session) {
    session.state = ChestSessionState::Opening;
    player.sendPacket(kPacketContainerOpen, containerOpenPayload(session.block_x, session.block_y, session.block_z));
    const auto ack = randomAckTimestamp();
    session.ack_timestamp = ack * 1000000;
    player.sendPacket(kPacketPing, pingPayload(ack));
}

void ExchangePlugin::sendChestContents(endstone::Player& player, ChestSession& session) {
    ChestRender render;
    auto& state = session.dashboard;
    auto all_categories = categories();
    if (all_categories.empty()) {
        return;
    }

    const auto category_exists = [&](const std::string& category_id) {
        return std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
            return category.id == category_id;
        });
    };
    if (state.search_query.empty() && !state.category_id.empty() && !category_exists(state.category_id)) {
        state.category_id.clear();
        state.category_page = 0;
        state.page = 0;
        state.product_key.clear();
    }

    const auto category_page_size = all_categories.size() > ui::chest_layout::kCategorySlots
                                        ? ui::chest_layout::kCategorySlots - 1
                                        : ui::chest_layout::kCategorySlots;
    const auto total_category_pages = std::max<std::size_t>(1, (all_categories.size() + category_page_size - 1) / category_page_size);
    if (state.category_page >= total_category_pages) {
        state.category_page = total_category_pages - 1;
    }
    if (!state.category_id.empty()) {
        auto active_it = std::find_if(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
            return category.id == state.category_id;
        });
        if (active_it != all_categories.end()) {
            state.category_page = static_cast<std::size_t>(std::distance(all_categories.begin(), active_it)) / category_page_size;
        }
    }
    const auto category_start = std::min(all_categories.size(), state.category_page * category_page_size);
    const auto category_end = std::min(all_categories.size(), category_start + category_page_size);

    std::vector<Product> products;
    std::string active_category_name;
    if (!state.search_query.empty()) {
        products = service_->searchCatalog(state.search_query);
        active_category_name = "搜索结果";
    } else if (state.category_id.empty()) {
        products = service_->getCatalog(std::nullopt);
        active_category_name = "全部物品";
    } else {
        products = service_->getCatalog(state.category_id);
        auto category_it = std::find_if(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
            return category.id == state.category_id;
        });
        active_category_name = category_it == all_categories.end() ? state.category_id : category_it->name;
    }

    const auto total_pages = std::max<std::size_t>(1, (products.size() + kDashboardPageSize - 1) / kDashboardPageSize);
    if (state.page >= total_pages) {
        state.page = total_pages - 1;
    }
    const auto start = std::min(products.size(), state.page * kDashboardPageSize);
    const auto end = std::min(products.size(), start + kDashboardPageSize);
    const auto selected_on_page = start < end && std::any_of(products.begin() + static_cast<std::ptrdiff_t>(start),
                                                             products.begin() + static_cast<std::ptrdiff_t>(end),
                                                             [&](const Product& product) {
                                                                 return product.product_key == state.product_key;
                                                             });
    if (!selected_on_page) {
        state.product_key = start < end ? products[start].product_key : "";
    }

    for (std::size_t slot = 0; slot < ui::chest_layout::kCategorySlots; ++slot) {
        const auto chest_slot = static_cast<int>(ui::chest_layout::kCategoryColumn[slot]);
        if (slot < category_end - category_start) {
            const auto& category = all_categories[category_start + slot];
            const bool active = state.search_query.empty() && state.category_id == category.id;
            render.items[chest_slot] = SlotItem{
                categoryIconItem(category),
                (active ? "当前分类: " : "分类: ") + category.name,
                {"点击切换分类", "分类组 " + std::to_string(state.category_page + 1) + "/" + std::to_string(total_category_pages)},
                1,
                active};
            render.actions[chest_slot] = {ChestActionKind::Category, category.id, 0};
        } else if (slot + 1 == ui::chest_layout::kCategorySlots && total_category_pages > 1) {
            const auto next_page = state.category_page + 1 < total_category_pages ? state.category_page + 1 : 0;
            render.items[chest_slot] = SlotItem{"minecraft:ender_pearl", "更多分类", {"下一组: " + std::to_string(next_page + 1) + "/" + std::to_string(total_category_pages)}, 1, false};
            render.actions[chest_slot] = {ChestActionKind::CategoryPage, "", next_page};
        }
    }

    for (std::size_t i = start; i < end; ++i) {
        const auto grid_index = i - start;
        const auto chest_slot = static_cast<int>(ui::chest_layout::kProductGrid[grid_index]);
        const auto& product = products[i];
        const auto quote = service_->getQuote(product.product_key);
        const bool selected = product.product_key == state.product_key;
        render.items[chest_slot] = SlotItem{
            product.item_id,
            (selected ? "已选: " : "") + product.display_name,
            {product.item_id,
             "买 " + money(quote.best_bid) + " / 卖 " + money(quote.best_ask),
             "成交 " + money(quote.last_price)},
            1,
            selected};
        render.actions[chest_slot] = {ChestActionKind::Product, product.product_key, 0};
    }

    const auto prev_slot = static_cast<int>(ui::chest_layout::kToolColumn[0]);
    render.items[prev_slot] = SlotItem{"minecraft:arrow", "上一页", {"商品页 " + std::to_string(state.page + 1) + "/" + std::to_string(total_pages)}, 1, false};
    if (state.page > 0) {
        render.actions[prev_slot] = {ChestActionKind::Page, "", state.page - 1};
    }
    const auto next_slot = static_cast<int>(ui::chest_layout::kToolColumn[1]);
    render.items[next_slot] = SlotItem{"minecraft:arrow", "下一页", {"商品页 " + std::to_string(state.page + 1) + "/" + std::to_string(total_pages)}, 1, false};
    if (state.page + 1 < total_pages) {
        render.actions[next_slot] = {ChestActionKind::Page, "", state.page + 1};
    }
    render.items[ui::chest_layout::kToolColumn[2]] = SlotItem{"minecraft:compass", "搜索", {"按名称或 item id 搜索"}, 1, false};
    render.actions[static_cast<int>(ui::chest_layout::kToolColumn[2])] = {ChestActionKind::Search, "", 0};
    render.items[ui::chest_layout::kToolColumn[3]] = SlotItem{"minecraft:book", "全部物品", {"显示完整商品目录"}, 1, false};
    render.actions[static_cast<int>(ui::chest_layout::kToolColumn[3])] = {ChestActionKind::AllProducts, "", 0};
    render.items[ui::chest_layout::kToolColumn[4]] = SlotItem{"minecraft:paper", "我的订单", {"查看和取消挂单"}, 1, false};
    render.actions[static_cast<int>(ui::chest_layout::kToolColumn[4])] = {ChestActionKind::MyOrders, "", 0};
    render.items[ui::chest_layout::kToolColumn[5]] = SlotItem{"minecraft:chest_minecart", "交易所邮箱", {"领取成交物品"}, 1, false};
    render.actions[static_cast<int>(ui::chest_layout::kToolColumn[5])] = {ChestActionKind::Mailbox, "", 0};

    std::optional<Product> selected_product;
    Quote selected_quote;
    if (!state.product_key.empty()) {
        selected_product = repository_->getProduct(state.product_key);
        if (selected_product) {
            selected_quote = service_->getQuote(selected_product->product_key);
        }
    }

    if (selected_product) {
        const auto& product = *selected_product;
        render.items[ui::chest_layout::kInfoSlot] = SlotItem{
            product.item_id,
            product.display_name,
            {"分类: " + active_category_name,
             "ID: " + product.item_id,
             "结果: " + std::to_string(products.size()) + " 个",
             "页: " + std::to_string(state.page + 1) + "/" + std::to_string(total_pages)},
            1,
            true};
        render.items[ui::chest_layout::kBookSlot] = SlotItem{"minecraft:book", "订单簿", {"查看买卖盘深度"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kBookSlot)] = {ChestActionKind::OrderBook, product.product_key, 0};
        render.items[ui::chest_layout::kBuyMarketSlot] = SlotItem{"minecraft:emerald", "买入 - 市价", {"按当前卖单立即买入"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kBuyMarketSlot)] = {ChestActionKind::MarketBuy, product.product_key, 0};
        render.items[ui::chest_layout::kBuyLimitSlot] = SlotItem{"minecraft:paper", "买入 - 挂单", {"输入数量和买价"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kBuyLimitSlot)] = {ChestActionKind::LimitBuy, product.product_key, 0};
        render.items[ui::chest_layout::kSellMarketSlot] = SlotItem{"minecraft:chest", "卖出 - 市价", {"从背包扣除并卖给买单"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kSellMarketSlot)] = {ChestActionKind::MarketSell, product.product_key, 0};
        render.items[ui::chest_layout::kSellLimitSlot] = SlotItem{"minecraft:writable_book", "卖出 - 挂单", {"输入数量和卖价"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kSellLimitSlot)] = {ChestActionKind::LimitSell, product.product_key, 0};
        render.items[ui::chest_layout::kBidSlot] = SlotItem{"minecraft:gold_ingot", "最高买价: " + money(selected_quote.best_bid), {"买方需求: " + std::to_string(selected_quote.bid_quantity)}, 1, false};
        render.items[ui::chest_layout::kAskSlot] = SlotItem{"minecraft:iron_ingot", "最低卖价: " + money(selected_quote.best_ask), {"卖方库存: " + std::to_string(selected_quote.ask_quantity)}, 1, false};
        render.items[ui::chest_layout::kDepthSlot] = SlotItem{"minecraft:spyglass", "差价: " + spread(selected_quote), {"买 " + money(selected_quote.best_bid) + " / 卖 " + money(selected_quote.best_ask)}, 1, false};
        render.items[ui::chest_layout::kLastSlot] = SlotItem{"minecraft:clock", "最近成交: " + money(selected_quote.last_price), {"点击商品刷新行情"}, 1, false};
    } else {
        render.items[ui::chest_layout::kInfoSlot] = SlotItem{"minecraft:barrier", "未选择商品", {"请从中间商品格选择"}, 1, false};
    }

    render.items[ui::chest_layout::kCloseSlot] = SlotItem{"minecraft:barrier", "关闭", {"关闭交易所"}, 1, false};
    render.actions[static_cast<int>(ui::chest_layout::kCloseSlot)] = {ChestActionKind::Close, "", 0};
    if (player.hasPermission("exchange.admin")) {
        render.items[ui::chest_layout::kAdminSlot] = SlotItem{"minecraft:gold_block", "管理员", {"系统补货入口"}, 1, false};
        render.actions[static_cast<int>(ui::chest_layout::kAdminSlot)] = {ChestActionKind::Admin, "", 0};
    }

    session.actions = std::move(render.actions);
    dashboard_states_[player.getUniqueId().str()] = state;
    player.sendPacket(kPacketInventoryContent, inventoryContentPayload(render.items, item_runtime_ids_));
    session.state = ChestSessionState::Open;
}

void ExchangePlugin::handleChestSlot(endstone::Player& player, int slot) {
    auto session_it = chest_sessions_.find(player.getUniqueId().str());
    if (session_it == chest_sessions_.end()) {
        return;
    }
    auto action_it = session_it->second.actions.find(slot);
    if (action_it == session_it->second.actions.end()) {
        sendChestContents(player, session_it->second);
        return;
    }
    const auto action = action_it->second;
    auto& state = dashboardState(player);
    state = session_it->second.dashboard;
    try {
        switch (action.kind) {
        case ChestActionKind::Category:
            state.category_id = action.target;
            state.search_query.clear();
            state.category_page = 0;
            state.page = 0;
            state.product_key.clear();
            search_queries_.erase(player.getUniqueId().str());
            refreshChest(player);
            return;
        case ChestActionKind::CategoryPage:
            state.category_page = action.page;
            refreshChest(player);
            return;
        case ChestActionKind::Product:
            state.product_key = action.target;
            refreshChest(player);
            return;
        case ChestActionKind::Page:
            state.page = action.page;
            state.product_key.clear();
            refreshChest(player);
            return;
        case ChestActionKind::Search:
            closeChest(player);
            openSearch(player);
            return;
        case ChestActionKind::AllProducts:
            state.category_id.clear();
            state.search_query.clear();
            state.category_page = 0;
            state.page = 0;
            state.product_key.clear();
            search_queries_.erase(player.getUniqueId().str());
            refreshChest(player);
            return;
        case ChestActionKind::MyOrders:
            closeChest(player);
            openMyOrders(player);
            return;
        case ChestActionKind::Mailbox:
            closeChest(player);
            openMailbox(player);
            return;
        case ChestActionKind::Admin:
            if (!player.hasPermission("exchange.admin")) {
                sendNotice(player, "没有管理员权限。");
                refreshChest(player);
                return;
            }
            closeChest(player);
            openAdmin(player);
            return;
        case ChestActionKind::MarketBuy:
            closeChest(player);
            openTradeForm(player, ui::ActionKind::MarketBuy, action.target);
            return;
        case ChestActionKind::LimitBuy:
            closeChest(player);
            openTradeForm(player, ui::ActionKind::LimitBuy, action.target);
            return;
        case ChestActionKind::MarketSell:
            closeChest(player);
            openTradeForm(player, ui::ActionKind::MarketSell, action.target);
            return;
        case ChestActionKind::LimitSell:
            closeChest(player);
            openTradeForm(player, ui::ActionKind::LimitSell, action.target);
            return;
        case ChestActionKind::OrderBook:
            closeChest(player);
            openOrderBook(player, action.target);
            return;
        case ChestActionKind::Close:
            closeChest(player);
            return;
        case ChestActionKind::None:
            refreshChest(player);
            return;
        }
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("箱子 UI 错误: ") + exc.what());
        refreshChest(player);
    }
}

void ExchangePlugin::handlePacketReceive(endstone::PacketReceiveEvent& event) {
    auto* player = event.getPlayer();
    if (player == nullptr) {
        return;
    }
    auto session_it = chest_sessions_.find(player->getUniqueId().str());
    if (session_it == chest_sessions_.end()) {
        return;
    }
    auto& session = session_it->second;
    const auto packet_id = event.getPacketId();
    if (packet_id == kPacketPing) {
        const auto timestamp = pingTimestampFromPayload(event.getPayload());
        if (!timestamp || *timestamp != session.ack_timestamp) {
            return;
        }
        if (session.state == ChestSessionState::GraphicSent) {
            sendChestGraphicData(*player, session);
        } else if (session.state == ChestSessionState::GraphicDataSent) {
            sendChestOpen(*player, session);
        } else if (session.state == ChestSessionState::Opening) {
            sendChestContents(*player, session);
        }
        return;
    }
    if (packet_id == kPacketPacketViolationWarning) {
        if (session.state == ChestSessionState::Opening) {
            sendChestContents(*player, session);
        }
        return;
    }
    if (packet_id == kPacketContainerClose) {
        PacketReader reader(event.getPayload());
        const auto container_id = reader.byte();
        if (container_id && *container_id == kChestContainerId) {
            event.cancel();
            closeChest(*player, true);
        }
        return;
    }
    if (packet_id == kPacketItemStackRequest && session.state == ChestSessionState::Open) {
        const auto slot = clickedSlotFromItemStackRequest(event.getPayload());
        if (!slot) {
            return;
        }
        event.cancel();
        handleChestSlot(*player, *slot);
    }
}

void ExchangePlugin::handlePacketSend(endstone::PacketSendEvent& event) {
    if (event.getPacketId() != kPacketItemRegistry || !item_runtime_ids_.empty()) {
        return;
    }
    auto parsed = itemRuntimeIdsFromRegistry(event.getPayload());
    if (!parsed.empty()) {
        item_runtime_ids_ = std::move(parsed);
        getLogger().info("Exchange cached " + std::to_string(item_runtime_ids_.size()) + " item runtime ids for chest UI");
    }
}

void ExchangePlugin::handlePlayerQuit(endstone::PlayerQuitEvent& event) {
    chest_sessions_.erase(event.getPlayer().getUniqueId().str());
}

void ExchangePlugin::openDashboard(endstone::Player& player) {
    openChestDashboard(player);
}

void ExchangePlugin::openDashboardCategory(endstone::Player& player, const std::string& category_id) {
    auto all_categories = categories();
    const auto exists = std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == category_id;
    });
    if (!exists) {
        sendNotice(player, "分类不存在。");
        openDashboard(player);
        return;
    }
    auto& state = dashboardState(player);
    state.category_id = category_id;
    state.search_query.clear();
    state.category_page = 0;
    state.page = 0;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openDashboardCategoryPage(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.category_page = page;
    openDashboard(player);
}

void ExchangePlugin::openDashboardProduct(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    auto& state = dashboardState(player);
    std::vector<Product> products;
    if (!state.search_query.empty()) {
        products = service_->searchCatalog(state.search_query);
    }
    auto it = std::find_if(products.begin(), products.end(), [&](const Product& candidate) {
        return candidate.product_key == product_key;
    });
    if (it == products.end()) {
        state.search_query.clear();
        search_queries_.erase(player.getUniqueId().str());
        state.category_id = product->category;
        state.category_page = 0;
        products = service_->getCatalog(state.category_id);
        it = std::find_if(products.begin(), products.end(), [&](const Product& candidate) {
            return candidate.product_key == product_key;
        });
    }
    state.product_key = product_key;
    if (it != products.end()) {
        const auto index = static_cast<std::size_t>(std::distance(products.begin(), it));
        state.page = index / kDashboardPageSize;
    }
    openDashboard(player);
}

void ExchangePlugin::openDashboardPage(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.page = page;
    state.product_key.clear();
    openDashboard(player);
}

void ExchangePlugin::resetDashboard(endstone::Player& player) {
    auto& state = dashboardState(player);
    state.category_id.clear();
    state.category_page = 0;
    state.page = 0;
    state.product_key.clear();
    state.search_query.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openCategory(endstone::Player& player, const std::string& category_id, std::size_t page) {
    const auto all_categories = categories();
    const auto exists = std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == category_id;
    });
    if (!exists) {
        sendNotice(player, "分类不存在。");
        openDashboard(player);
        return;
    }
    auto& state = dashboardState(player);
    state.category_id = category_id;
    state.search_query.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openAllProducts(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.category_id.clear();
    state.search_query.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openSearch(endstone::Player& player) {
    endstone::ModalForm form;
    form.setTitle(endstone::ColorFormat::Bold + endstone::ColorFormat::LightPurple + "搜索物品");
    form.addControl(endstone::TextInput(
        endstone::ColorFormat::Green + "输入物品名称或 ID",
        "diamond, stone, redstone, oak_log"));
    form.setSubmitButton(endstone::ColorFormat::Yellow + "搜索");
    form.setOnSubmit([this](endstone::Player* submitted, std::string json) {
        if (submitted == nullptr) {
            return;
        }
        const auto values = parseModalValues(json);
        if (values.empty() || trim(values[0]).empty()) {
            sendNotice(*submitted, "请输入搜索关键词。");
            openDashboard(*submitted);
            return;
        }
        auto& state = dashboardState(*submitted);
        state.search_query = trim(values[0]);
        state.category_id.clear();
        state.category_page = 0;
        state.page = 0;
        state.product_key.clear();
        search_queries_[submitted->getUniqueId().str()] = state.search_query;
        openDashboard(*submitted);
    });
    player.sendForm(std::move(form));
}

void ExchangePlugin::openSearchResults(endstone::Player& player, std::size_t page) {
    const auto it = search_queries_.find(player.getUniqueId().str());
    if (it == search_queries_.end() || it->second.empty()) {
        openSearch(player);
        return;
    }
    auto& state = dashboardState(player);
    state.search_query = it->second;
    state.category_id.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    openDashboard(player);
}

void ExchangePlugin::openProduct(endstone::Player& player, const std::string& product_key) {
    openDashboardProduct(player, product_key);
}

void ExchangePlugin::openOrderBook(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }
    sendForm(player, ui_.orderBookPage(*product, service_->listOrderBook(product_key, 12)));
}

void ExchangePlugin::openMyOrders(endstone::Player& player) {
    sendForm(player, ui_.myOrdersPage(service_->listPlayerOrders(playerRef(player), true)));
}

void ExchangePlugin::openMailbox(endstone::Player& player) {
    sendForm(player, ui_.mailboxPage(service_->listMailbox(playerRef(player))));
}

void ExchangePlugin::openAdmin(endstone::Player& player) {
    sendForm(player, ui_.adminHome(service_->getCatalog("building")));
}

void ExchangePlugin::openTradeForm(endstone::Player& player, ui::ActionKind action, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    const auto needs_price = action == ui::ActionKind::LimitBuy || action == ui::ActionKind::LimitSell;
    std::string title;
    switch (action) {
    case ui::ActionKind::MarketBuy:
        title = "市价买入";
        break;
    case ui::ActionKind::LimitBuy:
        title = "挂单买入";
        break;
    case ui::ActionKind::MarketSell:
        title = "市价卖出";
        break;
    case ui::ActionKind::LimitSell:
        title = "挂单卖出";
        break;
    default:
        title = "交易";
        break;
    }

    endstone::ModalForm form;
    form.setTitle(endstone::ColorFormat::Bold + endstone::ColorFormat::LightPurple + title + " - " + product->display_name);
    form.addControl(endstone::TextInput(
        endstone::ColorFormat::Green + "数量",
        "输入正整数",
        "1"));
    if (needs_price) {
        form.addControl(endstone::TextInput(
            endstone::ColorFormat::Green + "单价",
            "输入正整数",
            "1"));
    }
    form.setSubmitButton(endstone::ColorFormat::Yellow + "下一步");
    form.setOnSubmit([this, action, product_key, needs_price](endstone::Player* submitted, std::string json) {
        if (submitted == nullptr) {
            return;
        }
        const auto values = parseModalValues(json);
        if (values.empty()) {
            sendNotice(*submitted, "请输入数量。");
            openDashboardProduct(*submitted, product_key);
            return;
        }
        const auto quantity = parsePositiveInt32(values[0]);
        if (!quantity) {
            sendNotice(*submitted, "数量必须是正整数。");
            openDashboardProduct(*submitted, product_key);
            return;
        }
        std::int64_t unit_price = 0;
        if (needs_price) {
            if (values.size() < 2) {
                sendNotice(*submitted, "请输入单价。");
                openDashboardProduct(*submitted, product_key);
                return;
            }
            const auto price = parsePositiveInt64(values[1]);
            if (!price) {
                sendNotice(*submitted, "单价必须是正整数。");
                openDashboardProduct(*submitted, product_key);
                return;
            }
            unit_price = *price;
        }
        openTradeConfirm(*submitted, action, product_key, *quantity, unit_price);
    });
    player.sendForm(std::move(form));
}

void ExchangePlugin::openTradeConfirm(endstone::Player& player, ui::ActionKind action, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }
    ui::FormSpec form;
    std::ostringstream body;
    body << "确认交易"
         << "\n商品: " << product->display_name
         << "\n物品ID: " << product->item_id
         << "\n数量: " << quantity;
    if (unit_price > 0) {
        body << "\n单价: " << unit_price;
    }
    form.title = "Exchange Chest UI";
    form.body = body.str();
    form.buttons.push_back({"确认", "textures/ui/check", ui::ActionKind::ConfirmTrade, tradePayload(action, product_key, quantity, unit_price)});
    form.buttons.push_back({"返", "textures/ui/refresh_light", ui::ActionKind::Back, product_key});
    sendForm(player, ui_.fixedFrame(std::move(form)));
}

void ExchangePlugin::sendForm(endstone::Player& player, const ui::FormSpec& spec) {
    const auto framed_spec = ui_.fixedFrame(spec);
    endstone::ActionForm form;
    form.setTitle(framed_spec.title).setContent(framed_spec.body);
    const auto add_button = [this, &form](const ui::ButtonSpec& button) {
        if (button.text.empty() && button.action == ui::ActionKind::Noop) {
            return;
        }
        form.addButton(button.text, button.icon.empty() ? std::nullopt : std::optional<std::string>{button.icon},
                       [this, button](endstone::Player* clicked) {
                           if (clicked != nullptr) {
                               handleAction(*clicked, button);
                           }
                       });
    };
    if (!framed_spec.controls.empty()) {
        for (const auto& control : framed_spec.controls) {
            switch (control.kind) {
            case ui::ControlKind::Button:
                add_button(control.button);
                break;
            case ui::ControlKind::Header:
                form.addHeader(control.text);
                break;
            case ui::ControlKind::Label:
                form.addLabel(control.text);
                break;
            case ui::ControlKind::Divider:
                form.addDivider();
                break;
            }
        }
    } else {
        for (const auto& button : framed_spec.buttons) {
            add_button(button);
        }
    }
    player.sendForm(std::move(form));
}

void ExchangePlugin::handleAction(endstone::Player& player, const ui::ButtonSpec& button) {
    try {
        switch (button.action) {
        case ui::ActionKind::Noop:
            openDashboard(player);
            return;
        case ui::ActionKind::OpenDashboard:
            openDashboard(player);
            return;
        case ui::ActionKind::DashboardCategory:
            openDashboardCategory(player, button.target);
            return;
        case ui::ActionKind::DashboardCategoryPage: {
            const auto page = parsePageIndex(button.target);
            if (!page) {
                sendNotice(player, "分类页码无效。");
                openDashboard(player);
                return;
            }
            openDashboardCategoryPage(player, *page);
            return;
        }
        case ui::ActionKind::DashboardProduct:
            openDashboardProduct(player, button.target);
            return;
        case ui::ActionKind::DashboardPage: {
            const auto page = parsePageIndex(button.target);
            if (!page) {
                sendNotice(player, "页码无效。");
                openDashboard(player);
                return;
            }
            openDashboardPage(player, *page);
            return;
        }
        case ui::ActionKind::OpenAllProducts:
            resetDashboard(player);
            return;
        case ui::ActionKind::OpenSearch:
            openSearch(player);
            return;
        case ui::ActionKind::OpenCategory:
            openDashboardCategory(player, button.target);
            return;
        case ui::ActionKind::OpenProduct:
            openDashboardProduct(player, button.target);
            return;
        case ui::ActionKind::OpenOrderBook:
            openOrderBook(player, button.target);
            return;
        case ui::ActionKind::OpenAdmin:
            if (!player.hasPermission("exchange.admin")) {
                sendNotice(player, "没有管理员权限。");
                return;
            }
            openAdmin(player);
            return;
        case ui::ActionKind::AdminRestock: {
            if (!player.hasPermission("exchange.admin")) {
                sendNotice(player, "没有管理员权限。");
                return;
            }
            auto order = service_->adminCreateSystemSellOrder(playerRef(player), button.target, 64, 1);
            sendNotice(player, "已补货系统卖单 #" + std::to_string(order.id) + "，数量 64，单价 1。");
            openDashboardProduct(player, button.target);
            return;
        }
        case ui::ActionKind::MarketBuy:
        case ui::ActionKind::LimitBuy:
        case ui::ActionKind::MarketSell:
        case ui::ActionKind::LimitSell:
            if (!looksLikeProductKey(button.target)) {
                sendNotice(player, "请先从商品页选择交易方向。");
                return;
            }
            openTradeForm(player, button.action, button.target);
            return;
        case ui::ActionKind::ConfirmTrade:
            executeConfirmedTrade(player, button.target);
            return;
        case ui::ActionKind::OpenInventorySell:
        case ui::ActionKind::SellAll:
            sendNotice(player, "请先选择具体物品，再在商品面板卖出。");
            return;
        case ui::ActionKind::OpenMyOrders:
            openMyOrders(player);
            return;
        case ui::ActionKind::OpenMailbox:
            openMailbox(player);
            return;
        case ui::ActionKind::CancelOrder: {
            const auto order_id = parsePositiveInt64(button.target);
            if (!order_id) {
                sendNotice(player, "订单 ID 无效。");
                return;
            }
            const auto order = service_->cancelOrder(playerRef(player), *order_id);
            sendNotice(player, "已取消订单 #" + std::to_string(order.id) + "。");
            openMyOrders(player);
            return;
        }
        case ui::ActionKind::ClaimMailboxItem: {
            const auto mailbox_id = parsePositiveInt64(button.target);
            if (!mailbox_id) {
                sendNotice(player, "邮箱物品 ID 无效。");
                return;
            }
            claimMailboxItem(player, *mailbox_id);
            return;
        }
        case ui::ActionKind::ClaimAllMailbox:
            claimAllMailbox(player);
            return;
        case ui::ActionKind::Back:
            if (looksLikeProductKey(button.target)) {
                openDashboardProduct(player, button.target);
            } else {
                openDashboard(player);
            }
            return;
        case ui::ActionKind::NextPage:
        case ui::ActionKind::PrevPage: {
            const auto [target, page] = parsePageTarget(button.target);
            if (target == "search") {
                openDashboardPage(player, page);
            } else if (target == "all") {
                openAllProducts(player, page);
            } else {
                auto& state = dashboardState(player);
                state.category_id = target;
                state.search_query.clear();
                state.page = page;
                state.product_key.clear();
                openDashboard(player);
            }
            return;
        }
        }
    } catch (const ExchangeException& exc) {
        sendNotice(player, std::string("交易失败: ") + exc.what());
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("交易所错误: ") + exc.what());
    }
}

void ExchangePlugin::executeConfirmedTrade(endstone::Player& player, const std::string& payload) {
    const auto parts = split(payload, '|');
    if (parts.size() != 4) {
        sendNotice(player, "交易参数无效。");
        openDashboard(player);
        return;
    }
    const auto action = actionFromToken(parts[0]);
    const auto quantity = parsePositiveInt32(parts[2]);
    const auto unit_price = parsePositiveInt64(parts[3]);
    if (!action || !quantity) {
        sendNotice(player, "交易参数无效。");
        openDashboard(player);
        return;
    }

    const auto product = repository_->getProduct(parts[1]);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    const auto ref = playerRef(player);
    switch (*action) {
    case ui::ActionKind::MarketBuy: {
        const auto order = service_->placeMarketBuy(ref, product->product_key, *quantity);
        sendNotice(player, "市价买入完成，数量 " + std::to_string(order.original_quantity) + "，物品已进入交易所邮箱。");
        openDashboardProduct(player, product->product_key);
        return;
    }
    case ui::ActionKind::LimitBuy: {
        if (!unit_price) {
            sendNotice(player, "缺少买单单价。");
            openDashboardProduct(player, product->product_key);
            return;
        }
        const auto order = service_->placeLimitBuy(ref, product->product_key, *quantity, *unit_price);
        sendNotice(player, "已提交买单 #" + std::to_string(order.id) + "。");
        openDashboardProduct(player, product->product_key);
        return;
    }
    case ui::ActionKind::MarketSell:
    case ui::ActionKind::LimitSell: {
        if (!removeInventoryItems(player, *product, *quantity)) {
            openDashboardProduct(player, product->product_key);
            return;
        }
        try {
            Order order;
            if (*action == ui::ActionKind::MarketSell) {
                order = service_->placeMarketSell(ref, product->product_key, {snapshotFor(*product, *quantity)});
                sendNotice(player, "市价卖出完成，订单 #" + std::to_string(order.id) + "。");
            } else {
                if (!unit_price) {
                    throw InvalidOrder("missing sell price");
                }
                order = service_->placeLimitSell(ref, product->product_key, {snapshotFor(*product, *quantity)}, *unit_price);
                sendNotice(player, "已提交卖单 #" + std::to_string(order.id) + "。");
            }
            openDashboardProduct(player, product->product_key);
        } catch (...) {
            try {
                player.getInventory().addItem({endstone::ItemStack(endstone::ItemTypeId(product->item_id), *quantity)});
            } catch (...) {
                sendNotice(player, "交易失败且物品退回失败，请联系管理员。");
            }
            throw;
        }
        return;
    }
    default:
        sendNotice(player, "交易类型无效。");
        openDashboard(player);
        return;
    }
}

void ExchangePlugin::claimMailboxItem(endstone::Player& player, std::int64_t mailbox_id) {
    const auto item = repository_->getMailboxItem(mailbox_id);
    if (!item || item->player_uuid != player.getUniqueId().str() || item->claimed) {
        sendNotice(player, "邮箱物品不存在或已领取。");
        openMailbox(player);
        return;
    }
    if (!giveMailboxItem(player, *item)) {
        openMailbox(player);
        return;
    }
    const auto claimed = service_->markMailboxClaimed(playerRef(player), mailbox_id);
    sendNotice(player, "已领取邮箱物品 #" + std::to_string(claimed.id) + "，数量 " + std::to_string(claimed.quantity) + "。");
    openMailbox(player);
}

void ExchangePlugin::claimAllMailbox(endstone::Player& player) {
    auto items = service_->listMailbox(playerRef(player));
    std::int32_t claimed_count = 0;
    for (const auto& item : items) {
        if (!giveMailboxItem(player, item)) {
            break;
        }
        service_->markMailboxClaimed(playerRef(player), item.id);
        ++claimed_count;
    }
    sendNotice(player, "已领取 " + std::to_string(claimed_count) + " 个邮箱条目。");
    openMailbox(player);
}

ItemSnapshot ExchangePlugin::snapshotFor(const Product& product, std::int32_t quantity) const {
    return ItemSnapshot{product.product_key, product.item_id, product.enchants, quantity, {}, product.item_id};
}

bool ExchangePlugin::removeInventoryItems(endstone::Player& player, const Product& product, std::int32_t quantity) {
    try {
        auto& inventory = player.getInventory();
        if (!inventory.containsAtLeast(product.item_id, quantity)) {
            sendNotice(player, "背包内没有足够的 " + product.display_name + "。");
            return false;
        }
        auto leftovers = inventory.removeItem({endstone::ItemStack(endstone::ItemTypeId(product.item_id), quantity)});
        if (!leftovers.empty()) {
            sendNotice(player, "背包扣除失败，请重新检查物品数量。");
            return false;
        }
        return true;
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("背包扣除失败: ") + exc.what());
        return false;
    }
}

bool ExchangePlugin::giveMailboxItem(endstone::Player& player, const MailboxItem& item) {
    const auto product = repository_->getProduct(item.product_key);
    if (!product) {
        sendNotice(player, "邮箱中的商品不存在，无法领取。");
        return false;
    }
    try {
        auto leftovers = player.getInventory().addItem({endstone::ItemStack(endstone::ItemTypeId(product->item_id), item.quantity)});
        if (!leftovers.empty()) {
            sendNotice(player, "背包空间不足，领取已暂停。");
            return false;
        }
        return true;
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("领取失败: ") + exc.what());
        return false;
    }
}

void ExchangePlugin::sendNotice(endstone::Player& player, const std::string& message) {
    player.sendMessage("[交易所] " + message);
}

std::vector<ui::CategorySpec> ExchangePlugin::categories() const {
    std::vector<ui::CategorySpec> out;
    for (const auto& category : catalog::categories()) {
        out.push_back({category.id, category.name, category.icon});
    }
    return out;
}

DashboardState& ExchangePlugin::dashboardState(endstone::Player& player) {
    return dashboard_states_[player.getUniqueId().str()];
}

PlayerRef ExchangePlugin::playerRef(endstone::Player& player) const {
    return PlayerRef{player.getUniqueId().str(), player.getName()};
}

}  // namespace exchange::plugin

ENDSTONE_PLUGIN("exchange", "0.1.0", exchange::plugin::ExchangePlugin) {
    prefix = "Exchange";
    description = "C++ item exchange with MySQL order book and UMoney bridge";
    website = "https://github.com/wingxia/endstone-exchange";
    authors = {"wingxia"};

    command("ex")
        .description("Open the exchange")
        .usages("/ex")
        .permissions("exchange.command.ex");
    command("exchange")
        .description("Open or manage the exchange")
        .usages("/exchange [admin|reload]")
        .permissions("exchange.command.ex");

    permission("exchange.command.ex")
        .description("Allow players to open the exchange")
        .default_(endstone::PermissionDefault::True);
    permission("exchange.admin")
        .description("Allow administrators to manage system stock")
        .default_(endstone::PermissionDefault::Operator);
}
