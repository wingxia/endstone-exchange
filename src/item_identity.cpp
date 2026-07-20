#include "endstone_exchange/item_identity.hpp"

#include "endstone_exchange/nbt_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exchange {

namespace {

constexpr std::size_t MaxTextLength = 240;
constexpr std::size_t MaxListItems = 32;
constexpr std::size_t MaxExtraLines = 64;

std::string sanitizeText(const std::string_view value) {
    auto byte_limit = std::min(value.size(), MaxTextLength);
    while (byte_limit > 0 && byte_limit < value.size() &&
           (static_cast<unsigned char>(value[byte_limit]) & 0xc0U) == 0x80U) {
        --byte_limit;
    }
    std::string result;
    result.reserve(byte_limit);
    for (std::size_t index = 0; index < byte_limit; ++index) {
        const char character = value[index];
        if (character == '\r' || character == '\n' || character == '\t') {
            result += " ";
        } else {
            result.push_back(character);
        }
    }
    if (byte_limit < value.size()) {
        result += "…";
    }
    return result.empty() ? "空文本" : result;
}

std::optional<std::int64_t> integerValue(const endstone::nbt::Tag &tag) {
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

std::string enchantmentName(const std::int64_t id) {
    static constexpr std::array<std::string_view, 42> Names{
        "保护",     "火焰保护", "摔落缓冲", "爆炸保护", "弹射物保护", "荆棘",     "水下呼吸", "深海探索者", "水下速掘",
        "锋利",     "亡灵杀手", "节肢杀手", "击退",     "火焰附加",   "抢夺",     "效率",     "精准采集",   "耐久",
        "时运",     "力量",     "冲击",     "火矢",     "无限",       "海之眷顾", "饵钓",     "冰霜行者",   "经验修补",
        "绑定诅咒", "消失诅咒", "穿刺",     "激流",     "忠诚",       "引雷",     "多重箭",   "穿透",       "快速装填",
        "灵魂疾行", "迅捷潜行", "风爆",     "致密",     "破甲",       "突进",
    };
    if (id >= 0 && static_cast<std::size_t>(id) < Names.size()) {
        return std::string(Names[static_cast<std::size_t>(id)]);
    }
    return std::format("未知附魔编号 {}", id);
}

std::string friendlyNbtKey(const std::string_view key) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 22> Names{{
        {"CanPlaceOn", "可放置在"},
        {"CanDestroy", "可破坏方块"},
        {"chargedItem", "弩中装填物品"},
        {"BlockEntityTag", "方块内容"},
        {"EntityTag", "实体内容"},
        {"Trim", "盔甲纹饰"},
        {"Patterns", "旗帜图案"},
        {"Items", "内含物品"},
        {"pages", "书页"},
        {"author", "作者"},
        {"title", "标题"},
        {"resolved", "书本已解析"},
        {"map_uuid", "地图编号"},
        {"map_name_index", "地图名称编号"},
        {"customColor", "自定义颜色"},
        {"PotionId", "药水编号"},
        {"Name", "物品类型"},
        {"Count", "数量"},
        {"Damage", "数据值"},
        {"tag", "物品属性"},
        {"id", "编号"},
        {"lvl", "等级"},
    }};
    const auto found = std::ranges::find_if(Names, [key](const auto &entry) { return entry.first == key; });
    return found == Names.end() ? std::string(key) : std::string(found->second);
}

std::string scalarValue(const endstone::nbt::Tag &tag) {
    switch (tag.type()) {
    case endstone::nbt::Type::End:
        return "空";
    case endstone::nbt::Type::Byte:
        return std::to_string(tag.get<endstone::ByteTag>().value());
    case endstone::nbt::Type::Short:
        return std::to_string(tag.get<endstone::ShortTag>().value());
    case endstone::nbt::Type::Int:
        return std::to_string(tag.get<endstone::IntTag>().value());
    case endstone::nbt::Type::Long:
        return std::to_string(tag.get<endstone::LongTag>().value());
    case endstone::nbt::Type::Float:
        return std::format("{}", tag.get<endstone::FloatTag>().value());
    case endstone::nbt::Type::Double:
        return std::format("{}", tag.get<endstone::DoubleTag>().value());
    case endstone::nbt::Type::String:
        return sanitizeText(tag.get<endstone::StringTag>().value());
    default:
        return {};
    }
}

template <typename Array> std::string arrayValue(const Array &array) {
    std::string result;
    const auto shown = std::min(array.size(), MaxListItems);
    for (std::size_t index = 0; index < shown; ++index) {
        if (!result.empty()) {
            result += "、";
        }
        result += std::to_string(array[index]);
    }
    if (array.size() > shown) {
        result += std::format("，共 {} 项", array.size());
    }
    return result.empty() ? "空" : result;
}

std::string inlineListValue(const endstone::ListTag &list) {
    std::string result;
    const auto shown = std::min(list.size(), MaxListItems);
    for (std::size_t index = 0; index < shown; ++index) {
        const auto value = scalarValue(list[index]);
        if (value.empty()) {
            return {};
        }
        if (!result.empty()) {
            result += "、";
        }
        result += value;
    }
    if (list.size() > shown) {
        result += std::format("，共 {} 项", list.size());
    }
    return result.empty() ? "空" : result;
}

void appendExtraTag(const std::string &path, const endstone::nbt::Tag &tag, std::vector<std::string> &lines,
                    const std::size_t depth) {
    if (lines.size() >= MaxExtraLines) {
        return;
    }
    if (depth >= 8) {
        lines.emplace_back(path + "：内容层级较深，请使用同来源物品");
        return;
    }

    if (const auto scalar = scalarValue(tag); !scalar.empty()) {
        lines.emplace_back(path + "：" + scalar);
        return;
    }
    if (tag.type() == endstone::nbt::Type::ByteArray) {
        lines.emplace_back(path + "：" + arrayValue(tag.get<endstone::ByteArrayTag>()));
        return;
    }
    if (tag.type() == endstone::nbt::Type::IntArray) {
        lines.emplace_back(path + "：" + arrayValue(tag.get<endstone::IntArrayTag>()));
        return;
    }
    if (tag.type() == endstone::nbt::Type::List) {
        const auto &list = tag.get<endstone::ListTag>();
        if (const auto inline_value = inlineListValue(list); !inline_value.empty()) {
            lines.emplace_back(path + "：" + inline_value);
            return;
        }
        if (list.empty()) {
            lines.emplace_back(path + "：空");
            return;
        }
        for (std::size_t index = 0; index < list.size() && lines.size() < MaxExtraLines; ++index) {
            appendExtraTag(std::format("{} 第 {} 项", path, index + 1), list[index], lines, depth + 1);
        }
        return;
    }
    if (tag.type() == endstone::nbt::Type::Compound) {
        const auto &compound = tag.get<endstone::CompoundTag>();
        if (compound.empty()) {
            lines.emplace_back(path + "：空");
            return;
        }
        for (const auto &[key, value] : compound) {
            appendExtraTag(path + " · " + friendlyNbtKey(key), value, lines, depth + 1);
            if (lines.size() >= MaxExtraLines) {
                break;
            }
        }
    }
}

std::string join(const std::vector<std::string> &values, const std::string_view separator) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty()) {
            result += separator;
        }
        result += value;
    }
    return result;
}

} // namespace

bool matchesItemIdentity(const ItemPrototype &prototype, const std::string_view item_type, const int item_data,
                         const endstone::CompoundTag &item_nbt) {
    const auto expected_nbt = prototype.nbt.empty() ? endstone::CompoundTag{} : NbtCodec::decode(prototype.nbt);
    return item_type == prototype.type && item_data == prototype.data && item_nbt == expected_nbt;
}

std::string describeItemRequirements(const ItemPrototype &prototype) {
    auto remaining = prototype.nbt.empty() ? endstone::CompoundTag{} : NbtCodec::decode(prototype.nbt);
    std::vector<std::string> lines;
    lines.emplace_back("物品：" + prototype.name);
    lines.emplace_back(std::format("数据值：{}", prototype.data));

    std::string display_name = "无";
    std::vector<std::string> lore;
    if (remaining.contains("display") && remaining.at("display").type() == endstone::nbt::Type::Compound) {
        auto &display = remaining.at("display").get<endstone::CompoundTag>();
        if (display.contains("Name") && display.at("Name").type() == endstone::nbt::Type::String) {
            display_name = sanitizeText(display.at("Name").get<endstone::StringTag>().value()) + "§r";
            display.erase("Name");
        }
        if (display.contains("Lore") && display.at("Lore").type() == endstone::nbt::Type::List) {
            const auto &list = display.at("Lore").get<endstone::ListTag>();
            bool all_strings = true;
            for (const auto &entry : list) {
                if (entry.type() != endstone::nbt::Type::String) {
                    all_strings = false;
                    break;
                }
                lore.emplace_back(sanitizeText(entry.get<endstone::StringTag>().value()) + "§r");
            }
            if (all_strings) {
                display.erase("Lore");
            } else {
                lore.clear();
            }
        }
        if (display.empty()) {
            remaining.erase("display");
        }
    }
    lines.emplace_back("自定义名称：" + display_name);
    lines.emplace_back("物品说明：" + (lore.empty() ? std::string("无") : join(lore, "；")));

    std::vector<std::pair<std::int64_t, std::int64_t>> enchantments;
    if (remaining.contains("ench") && remaining.at("ench").type() == endstone::nbt::Type::List) {
        const auto &list = remaining.at("ench").get<endstone::ListTag>();
        bool valid = true;
        for (const auto &entry : list) {
            if (entry.type() != endstone::nbt::Type::Compound) {
                valid = false;
                break;
            }
            const auto &enchantment = entry.get<endstone::CompoundTag>();
            if (!enchantment.contains("id") || !enchantment.contains("lvl")) {
                valid = false;
                break;
            }
            const auto id = integerValue(enchantment.at("id"));
            const auto level = integerValue(enchantment.at("lvl"));
            if (!id || !level || enchantment.size() != 2) {
                valid = false;
                break;
            }
            enchantments.emplace_back(*id, *level & 0xffff);
        }
        if (valid) {
            remaining.erase("ench");
        } else {
            enchantments.clear();
        }
    }
    std::ranges::sort(enchantments);
    std::vector<std::string> enchantment_lines;
    enchantment_lines.reserve(enchantments.size());
    for (const auto &[id, level] : enchantments) {
        enchantment_lines.emplace_back(std::format("{} {}", enchantmentName(id), level));
    }
    lines.emplace_back("附魔：" + (enchantment_lines.empty() ? std::string("无") : join(enchantment_lines, "、")));

    std::int64_t damage = 0;
    if (remaining.contains("Damage")) {
        if (const auto value = integerValue(remaining.at("Damage"))) {
            damage = *value;
            remaining.erase("Damage");
        }
    }
    lines.emplace_back(std::format("耐久损耗：{}", damage));

    if (remaining.contains("RepairCost")) {
        if (const auto value = integerValue(remaining.at("RepairCost"))) {
            lines.emplace_back(std::format("铁砧修复代价：{}", *value));
            remaining.erase("RepairCost");
        }
    }
    if (remaining.contains("Unbreakable")) {
        if (const auto value = integerValue(remaining.at("Unbreakable"))) {
            lines.emplace_back(std::string("不会损坏：") + (*value == 0 ? "否" : "是"));
            remaining.erase("Unbreakable");
        }
    }

    std::vector<std::string> extra_lines;
    for (const auto &[key, value] : remaining) {
        appendExtraTag(friendlyNbtKey(key), value, extra_lines, 0);
        if (extra_lines.size() >= MaxExtraLines) {
            break;
        }
    }
    if (extra_lines.empty()) {
        lines.emplace_back("其他属性：无");
    } else {
        lines.emplace_back("其他属性：");
        for (const auto &line : extra_lines) {
            lines.emplace_back("- " + line);
        }
        if (extra_lines.size() >= MaxExtraLines) {
            lines.emplace_back("- 属性内容很多；未列出的部分也必须相同");
        }
    }
    return join(lines, "\n");
}

} // namespace exchange
