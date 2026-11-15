#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <optional>
#include <span>

namespace openxmb::xmb {

// Measurements use the same 1920x1080 logical canvas as the root XMB scene.
// On the console the frame is a glass capsule floating frame_inset in from
// the top-right corner of what is on screen.
struct StatusBarMetrics {
  static constexpr float logical_width = 1920.0F;
  static constexpr float logical_height = 1080.0F;

  static constexpr float frame_x = 1329.0F;
  static constexpr float frame_y = 75.0F;
  static constexpr float frame_width = 590.0F;
  static constexpr float frame_height = 57.0F;
  static constexpr float frame_corner_radius = 6.0F;
  static constexpr float frame_inset = 24.0F;

  [[nodiscard]] static constexpr float frame_right() noexcept { return logical_width - frame_inset; }
  // visible_top: the logical y of the top of the screen (negative when the
  // canvas is cropped).
  [[nodiscard]] static constexpr float frame_top(float visible_top) noexcept { return visible_top + frame_inset; }
  [[nodiscard]] static constexpr float frame_radius(float height) noexcept { return height * 0.5F; }

  static constexpr float icon_center_x = 1789.5F;
  static constexpr float icon_center_y = 103.5F;
  static constexpr float icon_radius = 16.0F;
  static constexpr float text_right_x = 1755.5F;
  // Play's visible cap height is slightly smaller than Rodin's at the same
  // nominal size, so the preserved identity font needs this measured native
  // fit to land on the reference glyph bounds.
  static constexpr float text_top_y = 85.0F;
  static constexpr float text_size = 29.0F;
};

struct StatusBarTime {
  int day{};
  int month{};
  int hour{};
  int minute{};
  int second{};

  friend constexpr bool operator==(const StatusBarTime &,
                                   const StatusBarTime &) noexcept = default;
};

// Live device state drawn to the left of the date/time inside the same
// top-right glass frame. The renderer refreshes it from Linux sysfs on a slow
// timer; every field keeps an "unknown" value so other platforms, captures and
// unit tests still render the clock alone.
struct StatusBarStatus {
  // Charge level 0..100, or -1 when no battery supply is readable.
  int battery_percent = -1;
  // The supply reports Charging, Full or a fast variant.
  bool charging = false;
  // -1: no wireless interface, indicator hidden. 0: interface present but
  // down or unassociated, bars drawn dim. 1..100: link quality in percent.
  int wifi_signal = -1;
};

struct ClockHandAngles {
  float hour_radians{};
  float minute_radians{};
};

[[nodiscard]] constexpr bool
valid_status_bar_time(const StatusBarTime &time) noexcept {
  return time.day >= 1 && time.day <= 31 && time.month >= 1 &&
         time.month <= 12 && time.hour >= 0 && time.hour <= 23 &&
         time.minute >= 0 && time.minute <= 59 && time.second >= 0 &&
         time.second <= 60;
}

// Writes the compact firmware-style form (for example "15/6 13:00") and a
// trailing NUL. Returns the character count excluding the NUL, or zero when
// either the value or destination is invalid. The function performs no heap
// allocation and never depends on the process locale.
[[nodiscard]] std::size_t
format_status_bar_time(const StatusBarTime &time,
                       std::span<char> destination) noexcept;

[[nodiscard]] StatusBarTime status_bar_time_from_tm(const std::tm &time) noexcept;

// Uses only the standard C++ local-time facility and copies its shared result
// while holding an internal lock. Callers can bypass the ambient timezone by
// passing StatusBarTime directly, which is the deterministic visual-test path.
[[nodiscard]] std::optional<StatusBarTime>
resolve_local_status_bar_time(
    std::chrono::system_clock::time_point time) noexcept;

[[nodiscard]] ClockHandAngles
resolve_clock_hand_angles(const StatusBarTime &time) noexcept;

} // namespace openxmb::xmb
