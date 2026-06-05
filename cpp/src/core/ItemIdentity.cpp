#include "endstone_exchange/core/ItemIdentity.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace exchange {
namespace {

std::uint32_t load32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void store64(std::uint64_t value, std::vector<std::uint8_t>& out) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU));
    }
}

constexpr std::array<std::uint32_t, 64> k{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

}  // namespace

std::string normalizeIdentifier(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c == ' ' ? '_' : static_cast<char>(std::tolower(c));
    });
    if (value.find(':') == std::string::npos && value != "air" && value != "none") {
        value = "minecraft:" + value;
    }
    return value;
}

std::string enchantSignature(std::vector<Enchantment> enchants) {
    for (auto& enchant : enchants) {
        enchant.id = normalizeIdentifier(enchant.id);
    }
    std::sort(enchants.begin(), enchants.end(), [](const Enchantment& lhs, const Enchantment& rhs) {
        return std::tie(lhs.id, lhs.level) < std::tie(rhs.id, rhs.level);
    });
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < enchants.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << "[\"" << enchants[i].id << "\"," << enchants[i].level << ']';
    }
    out << ']';
    return out.str();
}

std::string sha256Hex(const std::string& input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const auto bit_len = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) {
        data.push_back(0U);
    }
    store64(bit_len, data);

    std::array<std::uint32_t, 8> h{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    for (std::size_t offset = 0; offset < data.size(); offset += 64U) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = load32(data.data() + offset + i * 4U);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto a = h[0];
        auto b = h[1];
        auto c = h[2];
        auto d = h[3];
        auto e = h[4];
        auto f = h[5];
        auto g = h[6];
        auto hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = hh + s1 + ch + k[i] + w[i];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : h) {
        out << std::setw(8) << word;
    }
    return out.str();
}

std::string productKey(const std::string& item_id, std::vector<Enchantment> enchants) {
    const auto normalized = normalizeIdentifier(item_id);
    const auto payload = "{\"enchants\":" + enchantSignature(std::move(enchants)) + ",\"item_id\":\"" + normalized + "\"}";
    return sha256Hex(payload);
}

ItemSnapshot makeSnapshot(
    std::string item_id,
    std::vector<Enchantment> enchants,
    std::int32_t quantity,
    std::vector<std::uint8_t> nbt_blob,
    std::string nbt_summary) {
    if (quantity <= 0) {
        throw std::invalid_argument("snapshot quantity must be positive");
    }
    item_id = normalizeIdentifier(std::move(item_id));
    const auto key = productKey(item_id, enchants);
    return ItemSnapshot{key, item_id, std::move(enchants), quantity, std::move(nbt_blob), std::move(nbt_summary)};
}

}  // namespace exchange

