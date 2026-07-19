#pragma once

#include <cstdint>
#include <string>

namespace exchange {

// Bedrock's public entity API does not currently expose entity scale or
// collision-box metadata.  A tiny client-side actor keeps the nameplate
// visible without relying on invisibility, which also hides nameplates on
// Bedrock clients.
inline constexpr int SetActorDataPacketId = 39;

[[nodiscard]] std::string hologramAppearancePacket(std::uint64_t runtime_id);

} // namespace exchange
