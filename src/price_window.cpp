#include "endstone_exchange/price_window.hpp"

#include <algorithm>
#include <stdexcept>

namespace exchange {

Cents PriceSliderWindow::priceAt(const int index) const {
    if (index < 0 || index > max_index) {
        throw std::runtime_error("price slider index is outside its window");
    }
    return base_cents + step_cents * index;
}

PriceSliderWindow makePriceSliderWindow(const Cents minimum, const Cents maximum, const Cents step,
                                        const Cents reference, const int max_indices) {
    if (minimum <= 0 || maximum < minimum || step <= 0 || max_indices <= 0) {
        throw std::runtime_error("price slider limits are invalid");
    }
    const auto total_indices = (maximum - minimum) / step;
    const auto clamped_reference = std::clamp(reference, minimum, maximum);
    auto reference_index = (clamped_reference - minimum) / step;
    const auto remainder = (clamped_reference - minimum) % step;
    if (remainder >= step / 2 + step % 2 && reference_index < total_indices) {
        ++reference_index;
    }

    const auto visible_indices = std::min<Cents>(total_indices, max_indices);
    auto first_index = std::max<Cents>(0, reference_index - visible_indices / 2);
    if (first_index + visible_indices > total_indices) {
        first_index = total_indices - visible_indices;
    }

    PriceSliderWindow result;
    result.base_cents = minimum + first_index * step;
    result.step_cents = step;
    result.max_index = static_cast<int>(visible_indices);
    result.default_index = static_cast<int>(reference_index - first_index);
    return result;
}

} // namespace exchange
