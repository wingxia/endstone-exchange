#include "endstone_exchange/trade_form.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace exchange {
namespace {

std::string_view trim(const std::string_view text) noexcept {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

std::int64_t parseWholeNumber(const std::string_view raw, const std::int64_t minimum,
                              const std::int64_t maximum, const Message field, const Language language) {
    const auto text = trim(raw);
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value < minimum ||
        value > maximum) {
        throw UserError(tr(language, Message::WholeNumberRange, messageText(language, field), minimum, maximum));
    }
    return value;
}

} // namespace

bool tradeActionUsesPrice(const TradeAction action) noexcept {
    return action == TradeAction::LimitBuy || action == TradeAction::LimitSell;
}

Side tradeActionSide(const TradeAction action) noexcept {
    return action == TradeAction::LimitBuy || action == TradeAction::MarketBuy ? Side::Buy : Side::Sell;
}

OrderType tradeActionOrderType(const TradeAction action) noexcept {
    return tradeActionUsesPrice(action) ? OrderType::Limit : OrderType::Market;
}

std::string_view tradeActionLabel(const TradeAction action, const Language language) noexcept {
    switch (action) {
    case TradeAction::LimitBuy:
        return messageText(language, Message::TradeActionLimitBuy);
    case TradeAction::LimitSell:
        return messageText(language, Message::TradeActionLimitSell);
    case TradeAction::MarketBuy:
        return messageText(language, Message::TradeActionMarketBuy);
    case TradeAction::MarketSell:
        return messageText(language, Message::TradeActionMarketSell);
    }
    return messageText(language, Message::TradeActionFallback);
}

std::vector<TradeAction> availableTradeActions(const bool has_bids, const bool has_asks) {
    std::vector<TradeAction> result{TradeAction::LimitBuy, TradeAction::LimitSell};
    if (has_asks) {
        result.push_back(TradeAction::MarketBuy);
    }
    if (has_bids) {
        result.push_back(TradeAction::MarketSell);
    }
    return result;
}

Cents defaultTradePrice(const Cents minimum, const Cents maximum, const Cents step, const Cents reference) {
    if (minimum <= 0 || maximum < minimum || step <= 0) {
        throw std::runtime_error("invalid price range");
    }
    const auto clamped = std::clamp(reference, minimum, maximum);
    auto steps = (clamped - minimum) / step;
    const auto remainder = (clamped - minimum) % step;
    if (remainder >= step / 2 + step % 2 && minimum + steps * step <= maximum - step) {
        ++steps;
    }
    return minimum + steps * step;
}

std::vector<std::string> textFormValues(const std::string_view response, const Language language) {
    std::vector<std::string> values;
    std::size_t index = 0;
    const auto skip_whitespace = [&] {
        while (index < response.size() && std::isspace(static_cast<unsigned char>(response[index]))) {
            ++index;
        }
    };
    const auto invalid = [language] { throw UserError(tr(language, Message::InvalidFormData)); };

    skip_whitespace();
    if (index >= response.size() || response[index++] != '[') {
        invalid();
    }
    skip_whitespace();
    if (index < response.size() && response[index] == ']') {
        ++index;
        skip_whitespace();
        if (index != response.size()) {
            invalid();
        }
        return values;
    }

    bool array_closed = false;
    while (index < response.size()) {
        if (response[index++] != '"') {
            invalid();
        }
        std::string value;
        bool closed = false;
        while (index < response.size()) {
            const auto c = response[index++];
            if (c == '"') {
                closed = true;
                break;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                invalid();
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }
            if (index >= response.size()) {
                invalid();
            }
            switch (response[index++]) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                invalid();
            }
        }
        if (!closed) {
            invalid();
        }
        values.push_back(std::move(value));
        skip_whitespace();
        if (index >= response.size()) {
            invalid();
        }
        if (response[index] == ']') {
            ++index;
            array_closed = true;
            break;
        }
        if (response[index++] != ',') {
            invalid();
        }
        skip_whitespace();
    }
    skip_whitespace();
    if (!array_closed || index != response.size()) {
        invalid();
    }
    return values;
}

int parseTradeQuantity(const std::string_view text, const int maximum, const Language language) {
    if (maximum < 1) {
        throw UserError(tr(language, Message::QuantityRangeInvalid));
    }
    return static_cast<int>(parseWholeNumber(text, 1, maximum, Message::QuantityField, language));
}

Cents parseTradePrice(const std::string_view text, const Cents minimum, const Cents maximum, const Cents step,
                      const Language language) {
    if (minimum <= 0 || maximum < minimum || step <= 0 || minimum % 100 != 0 || maximum % 100 != 0 ||
        step % 100 != 0) {
        throw UserError(tr(language, Message::PriceRangeInvalid));
    }
    const auto units = parseWholeNumber(text, minimum / 100, maximum / 100, Message::UnitPriceField, language);
    if (units > std::numeric_limits<Cents>::max() / 100) {
        throw UserError(tr(language, Message::PriceTooLarge));
    }
    const auto cents = units * 100;
    if ((cents - minimum) % step != 0) {
        throw UserError(tr(language, Message::PriceStep, step / 100));
    }
    return cents;
}

int adjustTradeQuantity(const int current, const int delta, const int maximum) noexcept {
    const auto candidate = static_cast<long long>(current) + static_cast<long long>(delta);
    return static_cast<int>(std::clamp(candidate, 1LL, static_cast<long long>(std::max(maximum, 1))));
}

Cents adjustTradePrice(const Cents current, const int delta_units, const Cents minimum, const Cents maximum) noexcept {
    if (minimum > maximum) {
        return current;
    }
    const auto delta = static_cast<__int128>(delta_units) * 100;
    const auto candidate = static_cast<__int128>(current) + delta;
    return static_cast<Cents>(std::clamp(candidate, static_cast<__int128>(minimum), static_cast<__int128>(maximum)));
}

} // namespace exchange
