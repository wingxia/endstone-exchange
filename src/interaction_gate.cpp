#include "endstone_exchange/interaction_gate.hpp"

#include <stdexcept>

namespace exchange {

InteractionGate::InteractionGate(const std::chrono::milliseconds cooldown) : cooldown_(cooldown) {
    if (cooldown_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("interaction cooldown must be positive");
    }
}

bool InteractionGate::accept(const std::string_view player_uuid, const Clock::time_point now) {
    if (player_uuid.empty()) {
        return false;
    }
    const auto key = std::string(player_uuid);
    const auto previous = accepted_at_.find(key);
    if (previous != accepted_at_.end() && now - previous->second < cooldown_) {
        return false;
    }
    accepted_at_[key] = now;
    return true;
}

void InteractionGate::clear() {
    accepted_at_.clear();
}

} // namespace exchange
