#pragma once

#include <algorithm>
#include <string_view>

namespace openxmb::xmb {

// Game IDs are also directory names and systemd unit-name components.
inline bool valid_game_id(std::string_view id) noexcept {
  const auto alphanumeric = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
  };
  if (id.empty() || !alphanumeric(id.front())) return false;
  return std::all_of(id.begin() + 1, id.end(), [&](char c) {
    return alphanumeric(c) || c == '.' || c == '_' || c == '-';
  });
}

}  // namespace openxmb::xmb
