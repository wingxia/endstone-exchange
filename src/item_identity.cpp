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

constexpr std::size_t languageIndex(const Language language) noexcept {
    return static_cast<std::size_t>(language);
}

std::string_view listSeparator(const Language language) noexcept {
    return language == Language::English ? ", " : "、";
}

std::string_view descriptionSeparator(const Language language) noexcept {
    return language == Language::English ? "; " : language == Language::Japanese ? "、" : "；";
}

std::string sanitizeText(const std::string_view value, const Language language) {
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
    return result.empty() ? std::string(messageText(language, Message::ItemEmptyText)) : result;
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

std::string enchantmentName(const std::int64_t id, const Language language) {
    using LocalizedName = std::array<std::string_view, 4>;
    static constexpr std::array<LocalizedName, 42> Names{{
        {"保护", "Protection", "保護", "ダメージ軽減"},
        {"火焰保护", "Fire Protection", "火焰保護", "火炎耐性"},
        {"摔落缓冲", "Feather Falling", "摔落緩衝", "落下耐性"},
        {"爆炸保护", "Blast Protection", "爆炸保護", "爆発耐性"},
        {"弹射物保护", "Projectile Protection", "彈射物保護", "飛び道具耐性"},
        {"荆棘", "Thorns", "荊棘", "棘の鎧"},
        {"水下呼吸", "Respiration", "水下呼吸", "水中呼吸"},
        {"深海探索者", "Depth Strider", "深海漫遊者", "水中歩行"},
        {"水下速掘", "Aqua Affinity", "水下速掘", "水中採掘"},
        {"锋利", "Sharpness", "鋒利", "ダメージ増加"},
        {"亡灵杀手", "Smite", "不死剋星", "アンデッド特効"},
        {"节肢杀手", "Bane of Arthropods", "節肢剋星", "虫特効"},
        {"击退", "Knockback", "擊退", "ノックバック"},
        {"火焰附加", "Fire Aspect", "燃燒", "火属性"},
        {"抢夺", "Looting", "掠奪", "ドロップ増加"},
        {"效率", "Efficiency", "效率", "効率強化"},
        {"精准采集", "Silk Touch", "絲綢之觸", "シルクタッチ"},
        {"耐久", "Unbreaking", "耐久", "耐久力"},
        {"时运", "Fortune", "幸運", "幸運"},
        {"力量", "Power", "強力", "射撃ダメージ増加"},
        {"冲击", "Punch", "衝擊", "パンチ"},
        {"火矢", "Flame", "火焰", "フレイム"},
        {"无限", "Infinity", "無限", "無限"},
        {"海之眷顾", "Luck of the Sea", "海洋的祝福", "宝釣り"},
        {"饵钓", "Lure", "魚餌", "入れ食い"},
        {"冰霜行者", "Frost Walker", "冰霜行者", "氷渡り"},
        {"经验修补", "Mending", "修補", "修繕"},
        {"绑定诅咒", "Curse of Binding", "綁定詛咒", "束縛の呪い"},
        {"消失诅咒", "Curse of Vanishing", "消失詛咒", "消滅の呪い"},
        {"穿刺", "Impaling", "魚叉", "水生特効"},
        {"激流", "Riptide", "喚潮", "激流"},
        {"忠诚", "Loyalty", "忠誠", "忠誠"},
        {"引雷", "Channeling", "喚雷", "召雷"},
        {"多重箭", "Multishot", "多重射擊", "拡散"},
        {"穿透", "Piercing", "貫穿", "貫通"},
        {"快速装填", "Quick Charge", "快速裝填", "高速装填"},
        {"灵魂疾行", "Soul Speed", "靈魂疾走", "ソウルスピード"},
        {"迅捷潜行", "Swift Sneak", "迅捷潛行", "スニーク速度上昇"},
        {"风爆", "Wind Burst", "風爆", "ウィンドバースト"},
        {"致密", "Density", "緻密", "重撃"},
        {"破甲", "Breach", "破甲", "防具貫通"},
        {"突进", "Lunge", "突進", "突進"},
    }};
    if (id >= 0 && static_cast<std::size_t>(id) < Names.size()) {
        return std::string(Names[static_cast<std::size_t>(id)][languageIndex(language)]);
    }
    return tr(language, Message::UnknownEnchantment, id);
}

std::string friendlyNbtKey(const std::string_view key, const Language language) {
    struct NbtName {
        std::string_view key;
        std::array<std::string_view, 4> names;
    };
    static constexpr std::array<NbtName, 22> Names{{
        {"CanPlaceOn", {"可放置在", "Can be placed on", "可放置在", "設置可能なブロック"}},
        {"CanDestroy", {"可破坏方块", "Can break blocks", "可破壞方塊", "破壊可能なブロック"}},
        {"chargedItem", {"弩中装填物品", "Loaded crossbow item", "弩中裝填物品", "クロスボウ装填物"}},
        {"BlockEntityTag", {"方块内容", "Block contents", "方塊內容", "ブロック内容"}},
        {"EntityTag", {"实体内容", "Entity contents", "實體內容", "エンティティ内容"}},
        {"Trim", {"盔甲纹饰", "Armor trim", "盔甲紋飾", "防具装飾"}},
        {"Patterns", {"旗帜图案", "Banner patterns", "旗幟圖案", "旗の模様"}},
        {"Items", {"内含物品", "Contained items", "內含物品", "内包アイテム"}},
        {"pages", {"书页", "Book pages", "書頁", "本のページ"}},
        {"author", {"作者", "Author", "作者", "著者"}},
        {"title", {"标题", "Title", "標題", "タイトル"}},
        {"resolved", {"书本已解析", "Book resolved", "書本已解析", "本の解決状態"}},
        {"map_uuid", {"地图编号", "Map ID", "地圖編號", "地図ID"}},
        {"map_name_index", {"地图名称编号", "Map name ID", "地圖名稱編號", "地図名ID"}},
        {"customColor", {"自定义颜色", "Custom color", "自訂顏色", "カスタム色"}},
        {"PotionId", {"药水编号", "Potion ID", "藥水編號", "ポーションID"}},
        {"Name", {"物品类型", "Item type", "物品類型", "アイテム種別"}},
        {"Count", {"数量", "Quantity", "數量", "数量"}},
        {"Damage", {"数据值", "Data value", "資料值", "データ値"}},
        {"tag", {"物品属性", "Item properties", "物品屬性", "アイテム属性"}},
        {"id", {"编号", "ID", "編號", "ID"}},
        {"lvl", {"等级", "Level", "等級", "レベル"}},
    }};
    const auto found = std::ranges::find_if(Names, [key](const auto &entry) { return entry.key == key; });
    return found == Names.end() ? std::string(key) : std::string(found->names[languageIndex(language)]);
}

std::string scalarValue(const endstone::nbt::Tag &tag, const Language language) {
    switch (tag.type()) {
    case endstone::nbt::Type::End:
        return std::string(messageText(language, Message::ItemEmpty));
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
        return sanitizeText(tag.get<endstone::StringTag>().value(), language);
    default:
        return {};
    }
}

template <typename Array> std::string arrayValue(const Array &array, const Language language) {
    std::string result;
    const auto shown = std::min(array.size(), MaxListItems);
    for (std::size_t index = 0; index < shown; ++index) {
        if (!result.empty()) {
            result += listSeparator(language);
        }
        result += std::to_string(array[index]);
    }
    if (array.size() > shown) {
        result += tr(language, Message::ItemCountSuffix, array.size());
    }
    return result.empty() ? std::string(messageText(language, Message::ItemEmpty)) : result;
}

std::string inlineListValue(const endstone::ListTag &list, const Language language) {
    std::string result;
    const auto shown = std::min(list.size(), MaxListItems);
    for (std::size_t index = 0; index < shown; ++index) {
        const auto value = scalarValue(list[index], language);
        if (value.empty()) {
            return {};
        }
        if (!result.empty()) {
            result += listSeparator(language);
        }
        result += value;
    }
    if (list.size() > shown) {
        result += tr(language, Message::ItemCountSuffix, list.size());
    }
    return result.empty() ? std::string(messageText(language, Message::ItemEmpty)) : result;
}

void appendExtraTag(const std::string &path, const endstone::nbt::Tag &tag, std::vector<std::string> &lines,
                    const std::size_t depth, const Language language) {
    if (lines.size() >= MaxExtraLines) {
        return;
    }
    if (depth >= 8) {
        lines.emplace_back(tr(language, Message::ItemDeepValue, path));
        return;
    }

    if (const auto scalar = scalarValue(tag, language); !scalar.empty()) {
        lines.emplace_back(path + (language == Language::English ? ": " : "：") + scalar);
        return;
    }
    if (tag.type() == endstone::nbt::Type::ByteArray) {
        lines.emplace_back(path + (language == Language::English ? ": " : "：") +
                           arrayValue(tag.get<endstone::ByteArrayTag>(), language));
        return;
    }
    if (tag.type() == endstone::nbt::Type::IntArray) {
        lines.emplace_back(path + (language == Language::English ? ": " : "：") +
                           arrayValue(tag.get<endstone::IntArrayTag>(), language));
        return;
    }
    if (tag.type() == endstone::nbt::Type::List) {
        const auto &list = tag.get<endstone::ListTag>();
        if (const auto inline_value = inlineListValue(list, language); !inline_value.empty()) {
            lines.emplace_back(path + (language == Language::English ? ": " : "：") + inline_value);
            return;
        }
        if (list.empty()) {
            lines.emplace_back(path + (language == Language::English ? ": " : "：") +
                               std::string(messageText(language, Message::ItemEmpty)));
            return;
        }
        for (std::size_t index = 0; index < list.size() && lines.size() < MaxExtraLines; ++index) {
            appendExtraTag(tr(language, Message::ItemListEntry, path, index + 1), list[index], lines, depth + 1,
                           language);
        }
        return;
    }
    if (tag.type() == endstone::nbt::Type::Compound) {
        const auto &compound = tag.get<endstone::CompoundTag>();
        if (compound.empty()) {
            lines.emplace_back(path + (language == Language::English ? ": " : "：") +
                               std::string(messageText(language, Message::ItemEmpty)));
            return;
        }
        for (const auto &[key, value] : compound) {
            appendExtraTag(path + " · " + friendlyNbtKey(key, language), value, lines, depth + 1, language);
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

std::string describeItemRequirements(const ItemPrototype &prototype, const Language language,
                                     const std::string_view localized_item_name) {
    auto remaining = prototype.nbt.empty() ? endstone::CompoundTag{} : NbtCodec::decode(prototype.nbt);
    std::vector<std::string> lines;
    const auto item_name = localized_item_name.empty() ? std::string_view(prototype.name) : localized_item_name;
    lines.emplace_back(tr(language, Message::ItemLabel, item_name));
    lines.emplace_back(tr(language, Message::ItemData, prototype.data));

    std::string display_name(messageText(language, Message::NoneValue));
    std::vector<std::string> lore;
    if (remaining.contains("display") && remaining.at("display").type() == endstone::nbt::Type::Compound) {
        auto &display = remaining.at("display").get<endstone::CompoundTag>();
        if (display.contains("Name") && display.at("Name").type() == endstone::nbt::Type::String) {
            display_name = sanitizeText(display.at("Name").get<endstone::StringTag>().value(), language) + "§r";
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
                lore.emplace_back(sanitizeText(entry.get<endstone::StringTag>().value(), language) + "§r");
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
    lines.emplace_back(tr(language, Message::ItemCustomName, display_name));
    lines.emplace_back(tr(language, Message::ItemLore,
                          lore.empty() ? std::string(messageText(language, Message::NoneValue))
                                       : join(lore, descriptionSeparator(language))));

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
        enchantment_lines.emplace_back(std::format("{} {}", enchantmentName(id, language), level));
    }
    lines.emplace_back(tr(language, Message::ItemEnchantments,
                          enchantment_lines.empty() ? std::string(messageText(language, Message::NoneValue))
                                                    : join(enchantment_lines, listSeparator(language))));

    std::int64_t damage = 0;
    if (remaining.contains("Damage")) {
        if (const auto value = integerValue(remaining.at("Damage"))) {
            damage = *value;
            remaining.erase("Damage");
        }
    }
    lines.emplace_back(tr(language, Message::ItemDurabilityDamage, damage));

    if (remaining.contains("RepairCost")) {
        if (const auto value = integerValue(remaining.at("RepairCost"))) {
            lines.emplace_back(tr(language, Message::ItemRepairCost, *value));
            remaining.erase("RepairCost");
        }
    }
    if (remaining.contains("Unbreakable")) {
        if (const auto value = integerValue(remaining.at("Unbreakable"))) {
            lines.emplace_back(tr(language, Message::ItemUnbreakable,
                                  messageText(language, *value == 0 ? Message::NoValue : Message::YesValue)));
            remaining.erase("Unbreakable");
        }
    }

    std::vector<std::string> extra_lines;
    for (const auto &[key, value] : remaining) {
        appendExtraTag(friendlyNbtKey(key, language), value, extra_lines, 0, language);
        if (extra_lines.size() >= MaxExtraLines) {
            break;
        }
    }
    if (extra_lines.empty()) {
        lines.emplace_back(messageText(language, Message::ItemOtherNone));
    } else {
        lines.emplace_back(messageText(language, Message::ItemOtherHeader));
        for (const auto &line : extra_lines) {
            lines.emplace_back("- " + line);
        }
        if (extra_lines.size() >= MaxExtraLines) {
            lines.emplace_back(messageText(language, Message::ItemOtherMany));
        }
    }
    return join(lines, "\n");
}

} // namespace exchange
