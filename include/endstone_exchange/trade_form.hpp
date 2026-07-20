#pragma once

#include "endstone_exchange/domain.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace exchange {

enum class TradeAction { LimitBuy, LimitSell, MarketBuy, MarketSell };

struct TradeDraft {
    TradeAction action{TradeAction::LimitBuy};
    int quantity{1};
    Cents price_cents{0};
};

[[nodiscard]] bool tradeActionUsesPrice(TradeAction action) noexcept;
[[nodiscard]] Side tradeActionSide(TradeAction action) noexcept;
[[nodiscard]] OrderType tradeActionOrderType(TradeAction action) noexcept;
[[nodiscard]] std::string_view tradeActionLabel(TradeAction action) noexcept;
[[nodiscard]] std::vector<TradeAction> availableTradeActions(bool has_bids, bool has_asks);
[[nodiscard]] Cents defaultTradePrice(Cents minimum, Cents maximum, Cents step, Cents reference);

[[nodiscard]] std::vector<std::string> textFormValues(std::string_view response);
[[nodiscard]] int parseTradeQuantity(std::string_view text, int maximum);
[[nodiscard]] Cents parseTradePrice(std::string_view text, Cents minimum, Cents maximum, Cents step);
[[nodiscard]] int adjustTradeQuantity(int current, int delta, int maximum) noexcept;
[[nodiscard]] Cents adjustTradePrice(Cents current, int delta_units, Cents minimum, Cents maximum) noexcept;

} // namespace exchange
