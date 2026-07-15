#include "endstone_exchange/structure_reader.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace exchange {
namespace {

constexpr std::size_t MaxCaptureBytes = 64 * 1024 * 1024;
constexpr std::size_t LevelDbLogBlockBytes = 32 * 1024;
constexpr std::size_t LevelDbLogHeaderBytes = 7;

class BedrockNbtReader {
public:
    explicit BedrockNbtReader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    endstone::CompoundTag root()
    {
        const auto type = byte();
        if (type != 10) {
            throw std::runtime_error("mcstructure root is not an NBT compound");
        }
        static_cast<void>(string());
        auto result = payload(type);
        if (result.type() != endstone::nbt::Type::Compound) {
            throw std::runtime_error("mcstructure has an invalid NBT root");
        }
        return result.get<endstone::CompoundTag>();
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{0};

    void require(const std::size_t size) const
    {
        if (size > bytes_.size() - position_) {
            throw std::runtime_error("truncated mcstructure NBT");
        }
    }

    std::uint8_t byte()
    {
        require(1);
        return bytes_[position_++];
    }

    template <typename T>
        requires std::is_integral_v<T>
    T integer()
    {
        require(sizeof(T));
        using U = std::make_unsigned_t<T>;
        U raw = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            raw |= static_cast<U>(bytes_[position_++]) << (i * 8);
        }
        if constexpr (std::is_signed_v<T>) {
            return std::bit_cast<T>(raw);
        }
        return raw;
    }

    float floatValue() { return std::bit_cast<float>(integer<std::uint32_t>()); }
    double doubleValue() { return std::bit_cast<double>(integer<std::uint64_t>()); }

    std::string string()
    {
        const auto size = integer<std::uint16_t>();
        require(size);
        std::string result(reinterpret_cast<const char *>(bytes_.data() + position_), size);
        position_ += size;
        return result;
    }

    std::uint32_t containerLength()
    {
        const auto signed_size = integer<std::int32_t>();
        if (signed_size < 0 || signed_size > 16 * 1024 * 1024) {
            throw std::runtime_error("invalid mcstructure NBT container length");
        }
        return static_cast<std::uint32_t>(signed_size);
    }

    endstone::nbt::Tag payload(const std::uint8_t type)
    {
        switch (type) {
        case 0:
            return {};
        case 1:
            return endstone::ByteTag(byte());
        case 2:
            return endstone::ShortTag(integer<std::int16_t>());
        case 3:
            return endstone::IntTag(integer<std::int32_t>());
        case 4:
            return endstone::LongTag(integer<std::int64_t>());
        case 5:
            return endstone::FloatTag(floatValue());
        case 6:
            return endstone::DoubleTag(doubleValue());
        case 7:
            return byteArray();
        case 8:
            return endstone::StringTag(string());
        case 9:
            return list();
        case 10:
            return compound();
        case 11:
            return intArray();
        case 12:
            return longArrayAsList();
        default:
            throw std::runtime_error("unsupported mcstructure NBT type " + std::to_string(type));
        }
    }

    endstone::ByteArrayTag byteArray()
    {
        const auto size = containerLength();
        require(size);
        endstone::ByteArrayTag result(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
        position_ += size;
        return result;
    }

    endstone::IntArrayTag intArray()
    {
        const auto size = containerLength();
        endstone::IntArrayTag result;
        for (std::uint32_t i = 0; i < size; ++i) {
            result.push_back(integer<std::int32_t>());
        }
        return result;
    }

    endstone::ListTag longArrayAsList()
    {
        const auto size = containerLength();
        endstone::ListTag result;
        for (std::uint32_t i = 0; i < size; ++i) {
            result.emplace_back(endstone::LongTag(integer<std::int64_t>()));
        }
        return result;
    }

    endstone::ListTag list()
    {
        const auto element_type = byte();
        const auto size = containerLength();
        endstone::ListTag result;
        for (std::uint32_t i = 0; i < size; ++i) {
            result.emplace_back(payload(element_type));
        }
        return result;
    }

    endstone::CompoundTag compound()
    {
        endstone::CompoundTag result;
        while (true) {
            const auto type = byte();
            if (type == 0) {
                break;
            }
            auto name = string();
            result.insert_or_assign(name, payload(type));
        }
        return result;
    }
};

std::optional<std::int64_t> numeric(const endstone::nbt::Tag &tag)
{
    switch (tag.type()) {
    case endstone::nbt::Type::Byte:
        return tag.get<endstone::ByteTag>().value();
    case endstone::nbt::Type::Short:
        return tag.get<endstone::ShortTag>().value();
    case endstone::nbt::Type::Int:
        return tag.get<endstone::IntTag>().value();
    case endstone::nbt::Type::Long:
        return tag.get<endstone::LongTag>().value();
    default:
        return std::nullopt;
    }
}

std::optional<CapturedFrameItem> itemFromCompound(const endstone::CompoundTag &compound)
{
    if (!compound.contains("Item")) {
        return std::nullopt;
    }
    const auto &item_tag = compound.at("Item");
    if (item_tag.type() != endstone::nbt::Type::Compound) {
        return std::nullopt;
    }
    const auto &item = item_tag.get<endstone::CompoundTag>();
    if (!item.contains("Name") || item.at("Name").type() != endstone::nbt::Type::String) {
        return std::nullopt;
    }
    const auto type = item.at("Name").get<endstone::StringTag>().value();
    const auto count = item.contains("Count") ? numeric(item.at("Count")).value_or(0) : 0;
    if (count <= 0 || type.empty() || type == "minecraft:air") {
        return std::nullopt;
    }

    CapturedFrameItem result;
    result.type = type;
    if (item.contains("Damage")) {
        result.data = static_cast<int>(numeric(item.at("Damage")).value_or(0));
    }
    if (item.contains("tag") && item.at("tag").type() == endstone::nbt::Type::Compound) {
        result.nbt = item.at("tag").get<endstone::CompoundTag>();
    }
    return result;
}

std::optional<CapturedFrameItem> findItem(const endstone::nbt::Tag &tag, const int depth)
{
    if (depth > 32) {
        throw std::runtime_error("mcstructure NBT nesting is too deep");
    }
    if (tag.type() == endstone::nbt::Type::Compound) {
        const auto &compound = tag.get<endstone::CompoundTag>();
        if (const auto direct = itemFromCompound(compound)) {
            return direct;
        }
        for (const auto &[key, value] : compound) {
            static_cast<void>(key);
            if (const auto nested = findItem(value, depth + 1)) {
                return nested;
            }
        }
    }
    else if (tag.type() == endstone::nbt::Type::List) {
        for (const auto &value : tag.get<endstone::ListTag>()) {
            if (const auto nested = findItem(value, depth + 1)) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open structure data: " + path.string());
    }
    const auto end = input.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(MaxCaptureBytes)) {
        throw std::runtime_error("invalid structure data size: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("cannot read structure data: " + path.string());
    }
    return bytes;
}

std::optional<std::uint32_t> readVarUint32(const std::span<const std::uint8_t> bytes, std::size_t &position)
{
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (position >= bytes.size()) {
            return std::nullopt;
        }
        const auto byte = bytes[position++];
        if (shift == 28 && (byte & 0xf0U) != 0) {
            return std::nullopt;
        }
        result |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            return result;
        }
    }
    return std::nullopt;
}

std::uint32_t littleUint32(const std::span<const std::uint8_t> bytes, const std::size_t position)
{
    if (position > bytes.size() || bytes.size() - position < 4) {
        throw std::runtime_error("truncated LevelDB write batch");
    }
    return static_cast<std::uint32_t>(bytes[position]) |
           (static_cast<std::uint32_t>(bytes[position + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[position + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[position + 3]) << 24U);
}

void inspectWriteBatch(const std::span<const std::uint8_t> record, const std::string_view wanted_key,
                       std::optional<std::vector<std::uint8_t>> &value, bool &seen)
{
    if (record.size() < 12) {
        return;
    }
    const auto count = littleUint32(record, 8);
    std::size_t position = 12;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (position >= record.size()) {
            return;
        }
        const auto tag = record[position++];
        const auto key_size = readVarUint32(record, position);
        if (!key_size || *key_size > record.size() - position) {
            return;
        }
        const std::string_view key(reinterpret_cast<const char *>(record.data() + position), *key_size);
        position += *key_size;
        if (tag == 0) {
            if (key == wanted_key) {
                value.reset();
                seen = true;
            }
            continue;
        }
        if (tag != 1) {
            return;
        }
        const auto value_size = readVarUint32(record, position);
        if (!value_size || *value_size > record.size() - position) {
            return;
        }
        if (key == wanted_key) {
            value = std::vector<std::uint8_t>(record.begin() + static_cast<std::ptrdiff_t>(position),
                                              record.begin() + static_cast<std::ptrdiff_t>(position + *value_size));
            seen = true;
        }
        position += *value_size;
    }
}

void inspectLevelDbLog(const std::filesystem::path &path, const std::string_view wanted_key,
                       std::optional<std::vector<std::uint8_t>> &value, bool &seen)
{
    std::error_code size_error;
    if (std::filesystem::file_size(path, size_error) == 0 && !size_error) {
        return;
    }
    const auto bytes = readFile(path);
    std::vector<std::uint8_t> fragmented;
    bool collecting = false;
    for (std::size_t position = 0; position + LevelDbLogHeaderBytes <= bytes.size();) {
        const auto block_offset = position % LevelDbLogBlockBytes;
        const auto block_remaining = LevelDbLogBlockBytes - block_offset;
        if (block_remaining < LevelDbLogHeaderBytes) {
            position += block_remaining;
            continue;
        }
        const auto length = static_cast<std::size_t>(bytes[position + 4]) |
                            (static_cast<std::size_t>(bytes[position + 5]) << 8U);
        const auto record_type = bytes[position + 6];
        if (length == 0 && record_type == 0) {
            position += block_remaining;
            collecting = false;
            fragmented.clear();
            continue;
        }
        if (length > block_remaining - LevelDbLogHeaderBytes ||
            length > bytes.size() - position - LevelDbLogHeaderBytes) {
            break;
        }
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(position + LevelDbLogHeaderBytes);
        const auto end = begin + static_cast<std::ptrdiff_t>(length);
        const std::span<const std::uint8_t> fragment(bytes.data() + position + LevelDbLogHeaderBytes, length);
        position += LevelDbLogHeaderBytes + length;

        switch (record_type) {
        case 1:
            fragmented.clear();
            collecting = false;
            inspectWriteBatch(fragment, wanted_key, value, seen);
            break;
        case 2:
            fragmented.assign(begin, end);
            collecting = true;
            break;
        case 3:
            if (collecting && fragmented.size() + length <= MaxCaptureBytes) {
                fragmented.insert(fragmented.end(), begin, end);
            }
            else {
                collecting = false;
                fragmented.clear();
            }
            break;
        case 4:
            if (collecting && fragmented.size() + length <= MaxCaptureBytes) {
                fragmented.insert(fragmented.end(), begin, end);
                inspectWriteBatch(fragmented, wanted_key, value, seen);
            }
            collecting = false;
            fragmented.clear();
            break;
        default:
            collecting = false;
            fragmented.clear();
            break;
        }
    }
}

}  // namespace

std::optional<CapturedFrameItem> StructureReader::readItemFrame(const std::filesystem::path &path)
{
    const auto bytes = readFile(path);
    BedrockNbtReader reader(bytes);
    return findItem(endstone::nbt::Tag(reader.root()), 0);
}

std::optional<CapturedFrameItem>
StructureReader::readItemFrameFromLevelDbLogs(const std::filesystem::path &database_path,
                                              const std::string_view structure_name)
{
    std::vector<std::filesystem::path> logs;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(database_path, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file() && iterator->path().extension() == ".log") {
            logs.push_back(iterator->path());
        }
    }
    if (error) {
        throw std::runtime_error("cannot enumerate world LevelDB logs: " + error.message());
    }
    std::sort(logs.begin(), logs.end());
    const auto wanted_key = "structuretemplate_" + std::string(structure_name);
    std::optional<std::vector<std::uint8_t>> value;
    bool seen = false;
    for (const auto &log : logs) {
        inspectLevelDbLog(log, wanted_key, value, seen);
    }
    if (!seen || !value) {
        throw std::runtime_error("saved structure was not found in the world LevelDB log");
    }
    BedrockNbtReader reader(*value);
    return findItem(endstone::nbt::Tag(reader.root()), 0);
}

}  // namespace exchange
