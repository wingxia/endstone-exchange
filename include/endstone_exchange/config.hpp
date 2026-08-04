#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace exchange {

struct DatabaseConfig {
    std::string host{"127.0.0.1"};
    unsigned int port{3306};
    std::string user{"exchange"};
    std::string password;
    std::string name{"endstone_exchange"};
    unsigned int connect_timeout_seconds{5};
};

struct MarketConfig {
    std::int64_t initial_balance_cents{1'000'000};
    int max_order_quantity{640};
    std::int64_t price_min_cents{100};
    std::int64_t price_max_cents{500'000};
    std::int64_t price_step_cents{100};
    int order_book_depth{5};
    std::uint64_t hologram_refresh_ticks{20};
    std::uint64_t frame_capture_delay_ticks{80};
    bool cleanup_structure_captures{true};
};

struct EconomyConfig {
    std::string provider{"internal"};
    std::string umoney_plugin{"umoney"};
    std::uint64_t recovery_interval_ticks{100};

    [[nodiscard]] bool usesUmoney() const noexcept { return provider == "umoney"; }
};

struct Config {
    DatabaseConfig database;
    MarketConfig market;
    EconomyConfig economy;

    static Config load(const std::filesystem::path &path);
    static void writeTemplate(const std::filesystem::path &path);
    void validate() const;
};

}  // namespace exchange
