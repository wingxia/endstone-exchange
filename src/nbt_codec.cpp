#include "endstone_exchange/nbt_codec.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace exchange {
namespace {

class Writer {
public:
    template <typename T>
        requires std::is_integral_v<T>
    void integral(T value)
    {
        using U = std::make_unsigned_t<T>;
        U raw;
        if constexpr (std::is_signed_v<T>) {
            raw = std::bit_cast<U>(value);
        }
        else {
            raw = value;
        }
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((raw >> (i * 8)) & 0xffU));
        }
    }

    void floating(const float value) { integral(std::bit_cast<std::uint32_t>(value)); }
    void floating(const double value) { integral(std::bit_cast<std::uint64_t>(value)); }

    void string(const std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("NBT string is too large");
        }
        integral(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void tag(const endstone::nbt::Tag &tag)
    {
        integral(static_cast<std::uint8_t>(tag.type()));
        tag.visit([&](const auto &value) { payload(value); });
    }

    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;

    void payload(const std::monostate &) {}
    void payload(const endstone::ByteTag &value) { integral(value.value()); }
    void payload(const endstone::ShortTag &value) { integral(value.value()); }
    void payload(const endstone::IntTag &value) { integral(value.value()); }
    void payload(const endstone::LongTag &value) { integral(value.value()); }
    void payload(const endstone::FloatTag &value) { floating(value.value()); }
    void payload(const endstone::DoubleTag &value) { floating(value.value()); }
    void payload(const endstone::StringTag &value) { string(value.value()); }

    void payload(const endstone::ByteArrayTag &value)
    {
        integral(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void payload(const endstone::IntArrayTag &value)
    {
        integral(static_cast<std::uint32_t>(value.size()));
        for (const auto item : value) {
            integral(item);
        }
    }

    void payload(const endstone::ListTag &value)
    {
        integral(static_cast<std::uint32_t>(value.size()));
        for (const auto &item : value) {
            tag(item);
        }
    }

    void payload(const endstone::CompoundTag &value)
    {
        integral(static_cast<std::uint32_t>(value.size()));
        for (const auto &[key, item] : value) {
            string(key);
            tag(item);
        }
    }
};

class Reader {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    template <typename T>
        requires std::is_integral_v<T>
    T integral()
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

    float floatValue() { return std::bit_cast<float>(integral<std::uint32_t>()); }
    double doubleValue() { return std::bit_cast<double>(integral<std::uint64_t>()); }

    std::string string()
    {
        const auto size = length();
        require(size);
        std::string value(reinterpret_cast<const char *>(bytes_.data() + position_), size);
        position_ += size;
        return value;
    }

    endstone::nbt::Tag tag()
    {
        const auto type = static_cast<endstone::nbt::Type>(integral<std::uint8_t>());
        switch (type) {
        case endstone::nbt::Type::End:
            return {};
        case endstone::nbt::Type::Byte:
            return endstone::ByteTag(integral<std::uint8_t>());
        case endstone::nbt::Type::Short:
            return endstone::ShortTag(integral<std::int16_t>());
        case endstone::nbt::Type::Int:
            return endstone::IntTag(integral<std::int32_t>());
        case endstone::nbt::Type::Long:
            return endstone::LongTag(integral<std::int64_t>());
        case endstone::nbt::Type::Float:
            return endstone::FloatTag(floatValue());
        case endstone::nbt::Type::Double:
            return endstone::DoubleTag(doubleValue());
        case endstone::nbt::Type::String:
            return endstone::StringTag(string());
        case endstone::nbt::Type::ByteArray:
            return byteArray();
        case endstone::nbt::Type::IntArray:
            return intArray();
        case endstone::nbt::Type::List:
            return list();
        case endstone::nbt::Type::Compound:
            return compound();
        }
        throw std::runtime_error("unknown encoded NBT type");
    }

    [[nodiscard]] bool finished() const noexcept { return position_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{0};

    void require(const std::size_t count) const
    {
        if (count > bytes_.size() - position_) {
            throw std::runtime_error("truncated encoded NBT");
        }
    }

    std::uint32_t length()
    {
        const auto value = integral<std::uint32_t>();
        if (value > 16U * 1024U * 1024U) {
            throw std::runtime_error("encoded NBT container is too large");
        }
        return value;
    }

    endstone::ByteArrayTag byteArray()
    {
        const auto size = length();
        require(size);
        endstone::ByteArrayTag value(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                                     bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
        position_ += size;
        return value;
    }

    endstone::IntArrayTag intArray()
    {
        const auto size = length();
        endstone::IntArrayTag value;
        for (std::uint32_t i = 0; i < size; ++i) {
            value.push_back(integral<std::int32_t>());
        }
        return value;
    }

    endstone::ListTag list()
    {
        const auto size = length();
        endstone::ListTag value;
        for (std::uint32_t i = 0; i < size; ++i) {
            value.emplace_back(tag());
        }
        return value;
    }

    endstone::CompoundTag compound()
    {
        const auto size = length();
        endstone::CompoundTag value;
        for (std::uint32_t i = 0; i < size; ++i) {
            value.insert_or_assign(string(), tag());
        }
        return value;
    }
};

}  // namespace

std::vector<std::uint8_t> NbtCodec::encode(const endstone::CompoundTag &tag)
{
    Writer writer;
    writer.integral<std::uint8_t>('E');
    writer.integral<std::uint8_t>('X');
    writer.integral<std::uint8_t>('N');
    writer.integral<std::uint8_t>('B');
    writer.integral<std::uint8_t>(1);
    writer.tag(endstone::nbt::Tag(tag));
    return std::move(writer).take();
}

endstone::CompoundTag NbtCodec::decode(const std::span<const std::uint8_t> bytes)
{
    if (bytes.empty()) {
        return {};
    }
    Reader reader(bytes);
    if (reader.integral<std::uint8_t>() != 'E' || reader.integral<std::uint8_t>() != 'X' ||
        reader.integral<std::uint8_t>() != 'N' || reader.integral<std::uint8_t>() != 'B' ||
        reader.integral<std::uint8_t>() != 1) {
        throw std::runtime_error("invalid encoded NBT header");
    }
    auto value = reader.tag();
    if (!reader.finished() || value.type() != endstone::nbt::Type::Compound) {
        throw std::runtime_error("invalid encoded NBT root");
    }
    return value.get<endstone::CompoundTag>();
}

}  // namespace exchange
