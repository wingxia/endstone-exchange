#pragma once

#include "endstone_exchange/config.hpp"
#include "endstone_exchange/domain.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

namespace endstone {
class Server;
}

namespace exchange {

constexpr Cents UmoneyUnitCents = 100;

enum class UmoneyMutationDecision {
    Apply,
    AlreadyApplied,
    Conflict,
    Insufficient,
};

[[nodiscard]] constexpr UmoneyMutationDecision decideUmoneyMutation(
    EconomyTransferDirection direction, std::int64_t amount_units, std::int64_t balance_before_units,
    std::int64_t current_balance_units) noexcept {
    if (amount_units <= 0) {
        return UmoneyMutationDecision::Conflict;
    }

    std::int64_t expected_balance = 0;
    if (direction == EconomyTransferDirection::Deposit) {
        if (balance_before_units < amount_units) {
            return current_balance_units == balance_before_units ? UmoneyMutationDecision::Insufficient
                                                                 : UmoneyMutationDecision::Conflict;
        }
        expected_balance = balance_before_units - amount_units;
    } else {
        if (balance_before_units > std::numeric_limits<std::int64_t>::max() - amount_units) {
            return UmoneyMutationDecision::Conflict;
        }
        expected_balance = balance_before_units + amount_units;
    }

    if (current_balance_units == expected_balance) {
        return UmoneyMutationDecision::AlreadyApplied;
    }
    if (current_balance_units == balance_before_units) {
        return UmoneyMutationDecision::Apply;
    }
    return UmoneyMutationDecision::Conflict;
}

class UmoneyGateway {
  public:
    UmoneyGateway(endstone::Server &server, EconomyConfig config);
    ~UmoneyGateway();

    UmoneyGateway(const UmoneyGateway &) = delete;
    UmoneyGateway &operator=(const UmoneyGateway &) = delete;

    void validate() const;
    [[nodiscard]] std::optional<std::int64_t> balance(std::string_view player_name) const;
    void change(std::string_view player_name, std::int64_t delta_units) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace exchange
