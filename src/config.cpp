#include "endstone_exchange/config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace exchange {
namespace {

std::string trim(std::string value)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string unquote(std::string value)
{
    value = trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string decoded;
        decoded.reserve(value.size());
        bool escaped = false;
        for (const char c : value) {
            if (escaped) {
                switch (c) {
                case 'n':
                    decoded.push_back('\n');
                    break;
                case 'r':
                    decoded.push_back('\r');
                    break;
                case 't':
                    decoded.push_back('\t');
                    break;
                default:
                    decoded.push_back(c);
                    break;
                }
                escaped = false;
            }
            else if (c == '\\') {
                escaped = true;
            }
            else {
                decoded.push_back(c);
            }
        }
        if (escaped) {
            throw std::runtime_error("unterminated escape in quoted configuration value");
        }
        return decoded;
    }
    return value;
}

template <typename T>
T parseInteger(const std::string &value, std::string_view key)
{
    T result{};
    const auto text = trim(value);
    const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid integer for " + std::string(key));
    }
    return result;
}

bool parseBool(std::string value, std::string_view key)
{
    value = trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean for " + std::string(key));
}

std::int64_t parseCents(std::string value, std::string_view key)
{
    value = trim(std::move(value));
    if (value.empty()) {
        throw std::runtime_error("empty money value for " + std::string(key));
    }

    bool negative = false;
    if (value.front() == '+' || value.front() == '-') {
        negative = value.front() == '-';
        value.erase(value.begin());
    }
    const auto point = value.find('.');
    const std::string whole_text = point == std::string::npos ? value : value.substr(0, point);
    std::string fraction = point == std::string::npos ? "" : value.substr(point + 1);
    if (whole_text.empty() || fraction.size() > 2 || value.find('.', point == std::string::npos ? 0 : point + 1) !=
                                                        std::string::npos) {
        throw std::runtime_error("invalid money value for " + std::string(key));
    }
    while (fraction.size() < 2) {
        fraction.push_back('0');
    }
    const auto whole = parseInteger<std::int64_t>(whole_text, key);
    const auto minor = fraction.empty() ? 0 : parseInteger<std::int64_t>(fraction, key);
    if (whole > (std::numeric_limits<std::int64_t>::max() - minor) / 100) {
        throw std::runtime_error("money value is too large for " + std::string(key));
    }
    const auto cents = whole * 100 + minor;
    return negative ? -cents : cents;
}

}  // namespace

bool ProtectionConfig::contains(const std::string_view dimension_name, const int x, const int y,
                                const int z) const noexcept
{
    if (!enabled || dimension.size() != dimension_name.size() ||
        !std::equal(dimension.begin(), dimension.end(), dimension_name.begin(),
                    [](const unsigned char left, const unsigned char right) {
                        return std::tolower(left) == std::tolower(right);
                    })) {
        return false;
    }
    return x >= std::min(point1.x, point2.x) && x <= std::max(point1.x, point2.x) &&
           y >= std::min(point1.y, point2.y) && y <= std::max(point1.y, point2.y) &&
           z >= std::min(point1.z, point2.z) && z <= std::max(point1.z, point2.z);
}

Config Config::load(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("configuration file does not exist: " + path.string());
    }

    std::unordered_map<std::string, std::string> values;
    std::string section;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        bool quoted = false;
        bool escaped = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\' && quoted) {
                escaped = true;
                continue;
            }
            if (c == '"') {
                quoted = !quoted;
            }
            else if (c == '#' && !quoted) {
                line.resize(i);
                break;
            }
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("invalid configuration line " + std::to_string(line_number));
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        values[(section.empty() ? "" : section + ".") + key] = value;
    }

    Config config;
    const auto string_value = [&](const std::string &key, std::string fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : unquote(it->second);
    };
    const auto integer_value = [&]<typename T>(const std::string &key, T fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : parseInteger<T>(it->second, key);
    };
    const auto bool_value = [&](const std::string &key, bool fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : parseBool(it->second, key);
    };
    const auto cents_value = [&](const std::string &key, std::int64_t fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : parseCents(it->second, key);
    };

    config.database.host = string_value("database.host", config.database.host);
    config.database.port = integer_value("database.port", config.database.port);
    config.database.user = string_value("database.user", config.database.user);
    config.database.password = string_value("database.password", config.database.password);
    config.database.name = string_value("database.name", config.database.name);
    config.database.connect_timeout_seconds =
        integer_value("database.connect_timeout_seconds", config.database.connect_timeout_seconds);

    config.market.initial_balance_cents =
        cents_value("market.initial_balance", config.market.initial_balance_cents);
    config.market.max_order_quantity =
        integer_value("market.max_order_quantity", config.market.max_order_quantity);
    config.market.price_min_cents = cents_value("market.price_min", config.market.price_min_cents);
    config.market.price_max_cents = cents_value("market.price_max", config.market.price_max_cents);
    config.market.price_step_cents = cents_value("market.price_step", config.market.price_step_cents);
    config.market.order_book_depth = integer_value("market.order_book_depth", config.market.order_book_depth);
    config.market.hologram_refresh_ticks =
        integer_value("market.hologram_refresh_ticks", config.market.hologram_refresh_ticks);
    config.market.frame_capture_delay_ticks =
        integer_value("market.frame_capture_delay_ticks", config.market.frame_capture_delay_ticks);
    config.market.cleanup_structure_captures =
        bool_value("market.cleanup_structure_captures", config.market.cleanup_structure_captures);

    config.economy.provider = string_value("economy.provider", config.economy.provider);
    std::transform(config.economy.provider.begin(), config.economy.provider.end(), config.economy.provider.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    config.economy.umoney_plugin = string_value("economy.umoney_plugin", config.economy.umoney_plugin);
    config.economy.recovery_interval_ticks =
        integer_value("economy.recovery_interval_ticks", config.economy.recovery_interval_ticks);

    config.protection.enabled = bool_value("protection.enabled", config.protection.enabled);
    config.protection.dimension = string_value("protection.dimension", config.protection.dimension);
    config.protection.point1.x = integer_value("protection.point1_x", config.protection.point1.x);
    config.protection.point1.y = integer_value("protection.point1_y", config.protection.point1.y);
    config.protection.point1.z = integer_value("protection.point1_z", config.protection.point1.z);
    config.protection.point2.x = integer_value("protection.point2_x", config.protection.point2.x);
    config.protection.point2.y = integer_value("protection.point2_y", config.protection.point2.y);
    config.protection.point2.z = integer_value("protection.point2_z", config.protection.point2.z);
    if (config.protection.enabled) {
        constexpr std::array required_keys{
            "protection.dimension", "protection.point1_x", "protection.point1_y", "protection.point1_z",
            "protection.point2_x", "protection.point2_y", "protection.point2_z"};
        for (const auto *key : required_keys) {
            if (!values.contains(key)) {
                throw std::runtime_error("missing required protection setting " + std::string(key));
            }
        }
    }
    config.validate();
    return config;
}

void Config::writeTemplate(const std::filesystem::path &path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create configuration file: " + path.string());
    }
    output << R"config(# Endstone Exchange configuration
# Fill in the password locally. Do not commit this file.

[database]
host = "127.0.0.1"
port = 3306
user = "exchange"
password = ""
name = "endstone_exchange"
connect_timeout_seconds = 5

[market]
initial_balance = 10000.00
max_order_quantity = 640
price_min = 1
price_max = 5000
price_step = 1
order_book_depth = 5
hologram_refresh_ticks = 20
frame_capture_delay_ticks = 80
cleanup_structure_captures = true

[economy]
# UMoney mode calls the loaded UMoney plugin directly and requires initial_balance = 0.
provider = "internal"
umoney_plugin = "umoney"
recovery_interval_ticks = 100

[protection]
# Set enabled = true after replacing both points with opposite corners of the protected cuboid.
enabled = false
dimension = "Overworld"
point1_x = 0
point1_y = 0
point1_z = 0
point2_x = 0
point2_y = 0
point2_z = 0
)config";
}

void Config::validate() const
{
    if (database.host.empty() || database.user.empty() || database.name.empty()) {
        throw std::runtime_error("database host, user, and name must not be empty");
    }
    if (database.port == 0 || database.port > 65535 || database.connect_timeout_seconds == 0) {
        throw std::runtime_error("database port or timeout is invalid");
    }
    if (!std::all_of(database.name.begin(), database.name.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
        throw std::runtime_error("database name may contain only letters, digits, and underscores");
    }
    if (market.initial_balance_cents < 0 || market.max_order_quantity <= 0 || market.price_min_cents <= 0 ||
        market.price_max_cents < market.price_min_cents || market.price_step_cents <= 0 ||
        market.order_book_depth <= 0 || market.order_book_depth > 50 || market.hologram_refresh_ticks == 0 ||
        market.frame_capture_delay_ticks == 0) {
        throw std::runtime_error("market limits are invalid");
    }
    if (market.price_max_cents > std::numeric_limits<std::int64_t>::max() / market.max_order_quantity) {
        throw std::runtime_error("maximum order value is too large");
    }
    if (market.price_min_cents % 100 != 0 || market.price_max_cents % 100 != 0 ||
        market.price_step_cents % 100 != 0) {
        throw std::runtime_error("prices must use whole u units");
    }
    if (economy.provider != "internal" && economy.provider != "umoney") {
        throw std::runtime_error("economy.provider must be internal or umoney");
    }
    if (economy.recovery_interval_ticks == 0 || economy.recovery_interval_ticks > 72'000) {
        throw std::runtime_error("economy recovery interval is invalid");
    }
    if (economy.usesUmoney()) {
        if (economy.umoney_plugin.empty() || economy.umoney_plugin.find_first_of("\r\n") != std::string::npos) {
            throw std::runtime_error("economy.umoney_plugin must name a loaded plugin");
        }
        if (market.initial_balance_cents != 0) {
            throw std::runtime_error("UMoney mode requires market.initial_balance = 0");
        }
    }
    if (protection.enabled &&
        (protection.dimension.empty() || protection.dimension.find_first_of("\r\n") != std::string::npos)) {
        throw std::runtime_error("protection.dimension must name one dimension");
    }
}

}  // namespace exchange
