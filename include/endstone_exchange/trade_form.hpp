#pragma once

#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/localization.hpp"

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
[[nodiscard]] std::string_view tradeActionLabel(
    TradeAction action, Language language = Language::SimplifiedChinese) noexcept;
[[nodiscard]] std::vector<TradeAction> availableTradeActions(bool has_bids, bool has_asks);
[[nodiscard]] Cents defaultTradePrice(Cents minimum, Cents maximum, Cents step, Cents reference);

[[nodiscard]] std::vector<std::string> textFormValues(
    std::string_view response, Language language = Language::SimplifiedChinese);
[[nodiscard]] int parseTradeQuantity(std::string_view text, int maximum,
                                     Language language = Language::SimplifiedChinese);
[[nodiscard]] Cents parseTradePrice(std::string_view text, Cents minimum, Cents maximum, Cents step,
                                    Language language = Language::SimplifiedChinese);
[[nodiscard]] int adjustTradeQuantity(int current, int delta, int maximum) noexcept;
[[nodiscard]] Cents adjustTradePrice(Cents current, int delta_units, Cents minimum, Cents maximum) noexcept;

} // namespace exchange
