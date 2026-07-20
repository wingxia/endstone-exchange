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
                              const std::int64_t maximum, const std::string_view field) {
    const auto text = trim(raw);
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value < minimum ||
        value > maximum) {
        throw std::runtime_error(std::string(field) + "必须是 " + std::to_string(minimum) + " 至 " +
                                 std::to_string(maximum) + " 的整数");
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

std::string_view tradeActionLabel(const TradeAction action) noexcept {
    switch (action) {
    case TradeAction::LimitBuy:
        return "按我的价格购买";
    case TradeAction::LimitSell:
        return "按我的价格出售";
    case TradeAction::MarketBuy:
        return "直接购买";
    case TradeAction::MarketSell:
        return "直接出售";
    }
    return "交易";
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
        throw std::runtime_error("价格范围无效");
    }
    const auto clamped = std::clamp(reference, minimum, maximum);
    auto steps = (clamped - minimum) / step;
    const auto remainder = (clamped - minimum) % step;
    if (remainder >= step / 2 + step % 2 && minimum + steps * step <= maximum - step) {
        ++steps;
    }
    return minimum + steps * step;
}

std::vector<std::string> textFormValues(const std::string_view response) {
    std::vector<std::string> values;
    std::size_t index = 0;
    const auto skip_whitespace = [&] {
        while (index < response.size() && std::isspace(static_cast<unsigned char>(response[index]))) {
            ++index;
        }
    };
    const auto invalid = [] { throw std::runtime_error("客户端返回了无效表单数据"); };

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
            throw std::runtime_error("客户端返回了无效表单数据");
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

int parseTradeQuantity(const std::string_view text, const int maximum) {
    if (maximum < 1) {
        throw std::runtime_error("交易数量范围无效");
    }
    return static_cast<int>(parseWholeNumber(text, 1, maximum, "数量"));
}

Cents parseTradePrice(const std::string_view text, const Cents minimum, const Cents maximum, const Cents step) {
    if (minimum <= 0 || maximum < minimum || step <= 0 || minimum % 100 != 0 || maximum % 100 != 0 ||
        step % 100 != 0) {
        throw std::runtime_error("价格范围无效");
    }
    const auto units = parseWholeNumber(text, minimum / 100, maximum / 100, "每件价格");
    if (units > std::numeric_limits<Cents>::max() / 100) {
        throw std::runtime_error("每件价格超出允许范围");
    }
    const auto cents = units * 100;
    if ((cents - minimum) % step != 0) {
        throw std::runtime_error("每件价格必须按 " + std::to_string(step / 100) + "u 调整");
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
