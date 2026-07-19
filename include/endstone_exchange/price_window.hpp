#pragma once

#include "endstone_exchange/domain.hpp"

namespace exchange {

struct PriceSliderWindow {
    Cents base_cents{0};
    Cents step_cents{1};
    int max_index{0};
    int default_index{0};

    [[nodiscard]] Cents priceAt(int index) const;
    [[nodiscard]] Cents priceFromDisplayedUnits(double units) const;
};

[[nodiscard]] PriceSliderWindow makePriceSliderWindow(Cents minimum, Cents maximum, Cents step, Cents reference,
                                                      int max_indices = 10'000);

} // namespace exchange
