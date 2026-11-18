#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace app::display_refresh {

struct ModeInfo {
  int width{};
  int height{};
  int refresh_mhz{};
};

struct DisplayCapabilities {
  std::string output_name;
  std::vector<ModeInfo> modes;
  bool adaptive_sync{};
};

[[nodiscard]] DisplayCapabilities query_display_capabilities();
[[nodiscard]] std::vector<std::string>
available_refresh_rate_choices(const DisplayCapabilities &caps);
[[nodiscard]] bool is_valid_refresh_choice(std::string_view choice,
                                           const DisplayCapabilities &caps);
[[nodiscard]] int refresh_choice_target_hz(std::string_view choice) noexcept;
void apply_launcher_refresh_rate();
void apply_in_game_refresh_rate(std::string_view choice);

} // namespace app::display_refresh
