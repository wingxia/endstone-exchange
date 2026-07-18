#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>

namespace exchange {

class InteractionGate {
  public:
    using Clock = std::chrono::steady_clock;

    explicit InteractionGate(std::chrono::milliseconds cooldown);

    [[nodiscard]] bool accept(std::string_view player_uuid, Clock::time_point now = Clock::now());
    void clear();

  private:
    std::chrono::milliseconds cooldown_;
    std::unordered_map<std::string, Clock::time_point> accepted_at_;
};

} // namespace exchange
