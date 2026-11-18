module;
#include "openxmb/xmb/glass_draw.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "openxmb/xmb/status_bar.hpp"

export module openxmb.xmb.status_bar_renderer;

import auroreengine;
import glm;
import openxmb.config;

export namespace openxmb::xmb {

struct StatusBarIcons {
  const auroreengine::texture *battery_exclamation{};
  const auroreengine::texture *battery_1{};
  const auroreengine::texture *battery_2{};
  const auroreengine::texture *battery_3{};
  const auroreengine::texture *battery_4{};
  const auroreengine::texture *battery_charging{};
  const auroreengine::texture *battery_charging_2{};
  const auroreengine::texture *wifi{};
  const auroreengine::texture *wifi_2{};
  const auroreengine::texture *wifi_1{};
  const auroreengine::texture *wifi_0{};
  const auroreengine::texture *wifi_off{};
  const auroreengine::texture *aeroplane{};
  const auroreengine::texture *bluetooth{};
  const auroreengine::texture *bluetooth_connected{};
};

class StatusBarRenderer {
public:
  void set_icons(StatusBarIcons icons) noexcept { icons_ = icons; }

  void render(auroreengine::gui_renderer &renderer, const StatusBarTime &time,
              float opacity = 1.0F, std::string_view label = {},
              const StatusBarStatus &status = {}) const;

  // Convenience path for production. Deterministic tests and captures should
  // resolve their fixed civil time once and use the overload above.
  [[nodiscard]] bool
  render_local(auroreengine::gui_renderer &renderer,
               std::chrono::system_clock::time_point time,
               float opacity = 1.0F) const;

private:
  StatusBarIcons icons_{};
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

namespace glass = openxmb::xmb::glass;

struct ContainedLayout {
  float scale{};
  float offset_x{};
  float offset_y{};
  float framebuffer_width{};
  float framebuffer_height{};

  [[nodiscard]] float x(float logical_x) const noexcept {
    return (offset_x + logical_x * scale) / framebuffer_width;
  }
  [[nodiscard]] float y(float logical_y) const noexcept {
    return (offset_y + logical_y * scale) / framebuffer_height;
  }
  [[nodiscard]] float width(float logical_width) const noexcept {
    return logical_width * scale / framebuffer_width;
  }
  [[nodiscard]] float height(float logical_height) const noexcept {
    return logical_height * scale / framebuffer_height;
  }
};

[[nodiscard]] ContainedLayout
make_layout(const auroreengine::gui_renderer &renderer) noexcept {
  const auto width = static_cast<float>(renderer.frame_size.width);
  const auto height = static_cast<float>(renderer.frame_size.height);
  const auto base_scale = height / StatusBarMetrics::logical_height;
  const auto ui = static_cast<float>(config::CONFIG.ui_scale());
  const auto scale = base_scale * ui;
  const auto target_top =
      (ui > 1.0F) ? (height * 0.015F)
                  : (StatusBarMetrics::frame_y * base_scale);
  return {
      .scale = scale,
      .offset_x = width - StatusBarMetrics::logical_width * scale,
      .offset_y = target_top - StatusBarMetrics::frame_y * scale,
      .framebuffer_width = width,
      .framebuffer_height = height,
  };
}

void draw_logical_rect(auroreengine::gui_renderer &renderer,
                       const ContainedLayout &layout, float x, float y,
                       float width, float height, glm::vec4 color,
                       auroreengine::simple_params parameters = {}) {
  renderer.draw_rect({layout.x(x), layout.y(y)},
                     {layout.width(width), layout.height(height)}, color,
                     parameters);
}

// Device telemetry for the status indicators. sysfs is sampled on a slow timer
// so the render loop never touches the filesystem per frame; a missing node
// leaves its field unknown and the indicator is not drawn.
constexpr auto status_poll_interval = std::chrono::seconds(3);
constexpr const char *battery_supply_names[] = {
    "/sys/class/power_supply/qcom-battmgr-bat",
    "/sys/class/power_supply/battery",
};

[[nodiscard]] std::string
read_sysfs_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::string text;
  std::getline(stream, text);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

// Reads a leading unsigned integer and clamps it into 0..100; anything that
// does not start with a digit (empty node, "Unknown") is -1.
[[nodiscard]] int parse_status_percent(std::string_view text) noexcept {
  int value = 0;
  std::size_t digits = 0;
  for (const auto character : text) {
    if (character < '0' || character > '9') {
      break;
    }
    value = value * 10 + (character - '0');
    if (++digits > 3) {
      break;
    }
  }
  if (digits == 0) {
    return -1;
  }
  return std::clamp(value, 0, 100);
}

// The named supplies come from the Nova SoC nodes; any other Linux host falls
// back to the first supply whose type is Battery.
[[nodiscard]] std::filesystem::path battery_supply_path() {
  std::error_code error;
  for (const auto candidate : battery_supply_names) {
    if (std::filesystem::exists(std::filesystem::path{candidate} / "capacity",
                                error)) {
      return candidate;
    }
  }
  const std::filesystem::path root{"/sys/class/power_supply"};
  for (std::filesystem::directory_iterator entry{root, error}, end;
       entry != end; entry.increment(error)) {
    if (error) {
      break;
    }
    if (read_sysfs_text(entry->path() / "type") == "Battery") {
      return entry->path();
    }
  }
  return {};
}

// The third numeric field of a /proc/net/wireless row is the driver's level in
// dBm; it is smoothed and negative, for example "wlan0: 0000 64. -46. -256".
[[nodiscard]] std::optional<int> wireless_level_from_row(std::string_view row) noexcept {
  int numeric_fields = 0;
  std::size_t index = 0;
  while (index < row.size()) {
    while (index < row.size() && row[index] != '-' &&
           (row[index] < '0' || row[index] > '9')) {
      ++index;
    }
    if (index >= row.size()) {
      break;
    }
    const auto negative = row[index] == '-';
    if (negative) {
      ++index;
    }
    int magnitude = 0;
    std::size_t digits = 0;
    while (index < row.size() && row[index] >= '0' && row[index] <= '9') {
      magnitude = magnitude * 10 + (row[index] - '0');
      ++index;
      ++digits;
    }
    if (index < row.size() && row[index] == '.') {
      ++index;
      while (index < row.size() && row[index] >= '0' && row[index] <= '9') {
        ++index;
      }
    }
    if (digits == 0) {
      continue;
    }
    if (++numeric_fields == 3) {
      return negative ? -magnitude : magnitude;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<int> wireless_level_dbm(std::string_view interface) {
  std::ifstream stream("/proc/net/wireless");
  if (!stream) {
    return std::nullopt;
  }
  std::string line;
  while (std::getline(stream, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto name = std::string_view{line}.substr(0, colon);
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
      name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
      name.remove_suffix(1);
    }
    if (name == interface) {
      return wireless_level_from_row(
          std::string_view{line}.substr(colon + 1));
    }
  }
  return std::nullopt;
}

// -100 dBm is an unusable link and -50 dBm or better is full bars.
[[nodiscard]] int wireless_signal_from_dbm(int dbm) noexcept {
  constexpr int empty_dbm = -100;
  constexpr int full_dbm = -50;
  return std::clamp((dbm - empty_dbm) * 100 / (full_dbm - empty_dbm), 0, 100);
}

// The first wireless interface in sysfs decides the indicator: an "up" link
// means associated, a down link keeps the icon dim, and no wireless
// interface at all hides it.
[[nodiscard]] std::string active_wifi_interface(bool &wireless_present) {
  std::error_code error;
  const std::filesystem::path net_root{"/sys/class/net"};
  wireless_present = false;
  for (std::filesystem::directory_iterator entry{net_root, error}, end;
       entry != end; entry.increment(error)) {
    if (error) {
      break;
    }
    const auto name = entry->path().filename().string();
    if (name.rfind("wl", 0) != 0) {
      continue;
    }
    wireless_present = true;
    if (read_sysfs_text(entry->path() / "operstate") == "up") {
      return name;
    }
  }
  return {};
}

[[nodiscard]] int read_wifi_signal(std::string &connected_iface) {
  bool wireless_present = false;
  connected_iface = active_wifi_interface(wireless_present);
  if (!wireless_present) {
    return -1;
  }
  if (connected_iface.empty()) {
    return 0;
  }
  const auto level = wireless_level_dbm(connected_iface);
  // A cfg80211 driver that exports no legacy level is still associated, so
  // show full strength rather than a false weak link.
  return level.has_value() ? wireless_signal_from_dbm(*level) : 100;
}

[[nodiscard]] bool has_default_route_for(std::string_view iface) {
  if (iface.empty()) {
    return false;
  }
  {
    std::ifstream route("/proc/net/route");
    std::string line;
    std::getline(route, line);
    while (std::getline(route, line)) {
      const auto tab1 = line.find('\t');
      if (tab1 == std::string::npos) continue;
      if (std::string_view{line}.substr(0, tab1) != iface) continue;
      const auto tab2 = line.find('\t', tab1 + 1);
      if (tab2 == std::string::npos) continue;
      if (std::string_view{line}.substr(tab1 + 1, tab2 - tab1 - 1) == "00000000") {
        return true;
      }
    }
  }
  {
    std::ifstream route6("/proc/net/ipv6_route");
    std::string line;
    while (std::getline(route6, line)) {
      // Columns: dest dst_len src src_len next_hop metric ref use flags iface
      if (line.size() < iface.size() || !line.ends_with(iface)) continue;
      if (line.starts_with("00000000000000000000000000000000 00 ")) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool check_wifi_no_internet(const std::string &iface) {
  if (iface.empty() || !has_default_route_for(iface)) {
    return true;
  }
  static std::atomic<bool> internet_ok{true};
  static std::atomic<bool> probe_running{false};
  static std::optional<std::chrono::steady_clock::time_point> last_probe;
  static std::string last_iface;
  const auto now = std::chrono::steady_clock::now();
  const bool iface_changed = last_iface != iface;
  const bool due = iface_changed || !last_probe.has_value() ||
                   (now - *last_probe >= std::chrono::seconds(15));
  if (due && !probe_running.exchange(true)) {
    last_iface = iface;
    last_probe = now;
    std::thread([iface]() {
      internet_ok.store(probe_wifi_internet(iface), std::memory_order_relaxed);
      probe_running.store(false, std::memory_order_relaxed);
    }).detach();
  }
  return !internet_ok.load(std::memory_order_relaxed);
}

[[nodiscard]] bool any_external_power_online() {
  std::error_code error;
  const std::filesystem::path root{"/sys/class/power_supply"};
  for (std::filesystem::directory_iterator entry{root, error}, end;
       entry != end; entry.increment(error)) {
    if (error) {
      break;
    }
    const auto type = read_sysfs_text(entry->path() / "type");
    if (type == "Battery") {
      continue;
    }
    if (read_sysfs_text(entry->path() / "online") == "1") {
      return true;
    }
  }
  return false;
}

// rfkill exposes each radio as /sys/class/rfkill/rfkillN/type and /state,
// where 0 is blocked and anything else is unblocked. A missing node leaves the
// radio unknown (and its glyph hidden) rather than showing a false indicator.
[[nodiscard]] bool radio_unblocked(std::string_view wanted_type) {
  std::error_code error;
  const std::filesystem::path root{"/sys/class/rfkill"};
  for (std::filesystem::directory_iterator entry{root, error}, end;
       entry != end; entry.increment(error)) {
    const auto type = read_sysfs_text(entry->path() / "type");
    if (type != wanted_type) {
      continue;
    }
    const auto state = read_sysfs_text(entry->path() / "state");
    return !state.empty() && state != "0";
  }
  return false;
}

// Active link-layer Bluetooth connections appear as hciN:handle under
// /sys/class/bluetooth; scanning alone does not create them.
[[nodiscard]] bool read_bluetooth_connected() {
  std::error_code error;
  const std::filesystem::path root{"/sys/class/bluetooth"};
  for (std::filesystem::directory_iterator entry{root, error}, end;
       entry != end; entry.increment(error)) {
    if (error) {
      break;
    }
    const auto name = entry->path().filename().string();
    if (name.find(':') != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] StatusBarStatus read_status_bar_status() {
  StatusBarStatus status{};
  const auto supply = battery_supply_path();
  if (!supply.empty()) {
    status.battery_percent =
        parse_status_percent(read_sysfs_text(supply / "capacity"));
    const auto state = read_sysfs_text(supply / "status");
    const bool plugged = state == "Full" || state.starts_with("Charg") ||
                         any_external_power_online();
    status.charging = state == "Full" || state.starts_with("Charg") ||
                      (plugged && status.battery_percent >= 100);
  }
  std::string wifi_iface;
  status.wifi_signal = read_wifi_signal(wifi_iface);
  status.wifi_no_internet =
      status.wifi_signal > 0 && check_wifi_no_internet(wifi_iface);
  const bool wifi_on = radio_unblocked("wlan");
  status.bluetooth_on = radio_unblocked("bluetooth");
  status.bluetooth_connected = status.bluetooth_on && read_bluetooth_connected();
  status.aeroplane_on = !wifi_on && !status.bluetooth_on;
  return status;
}

[[nodiscard]] const StatusBarStatus &polled_status_bar_status() {
  static StatusBarStatus cached{};
  static std::optional<std::chrono::steady_clock::time_point> refreshed_at;
  const auto now = std::chrono::steady_clock::now();
  if (!refreshed_at.has_value() || now - *refreshed_at >= status_poll_interval) {
    cached = read_status_bar_status();
    refreshed_at = now;
  }
  return cached;
}

// One framebuffer pixel in logical units: the 4:3 canvas scales down, and a
// thinner outline would smear the battery frame and the Wi-Fi bars.
[[nodiscard]] float logical_hairline(const ContainedLayout &layout) noexcept {
  return std::max(0.75F, 1.0F / std::max(0.5F, layout.scale));
}

constexpr std::size_t kRingSegments = 32;

void draw_ring(auroreengine::gui_renderer &renderer,
               const ContainedLayout &layout, float center_x, float center_y,
               float radius, float width, glm::vec4 color) {
  std::array<auroreengine::simple_renderer::vertex_data, kRingSegments * 6>
      vertices{};
  constexpr auto pi = 3.14159265358979323846F;
  constexpr auto full_turn = 2.0F * pi;
  const auto inner = std::max(0.0F, radius - width * 0.5F);
  const auto outer = radius + width * 0.5F;

  const auto vertex = [&](float x, float y) {
    return auroreengine::simple_renderer::vertex_data{
        .position = {layout.x(x), layout.y(y)},
        .color = color,
        .tex_coords = {},
    };
  };
  for (std::size_t segment = 0; segment < kRingSegments; ++segment) {
    const auto angle0 = full_turn * static_cast<float>(segment) /
                        static_cast<float>(kRingSegments);
    const auto angle1 = full_turn * static_cast<float>(segment + 1) /
                        static_cast<float>(kRingSegments);
    const auto cos0 = std::cos(angle0);
    const auto sin0 = std::sin(angle0);
    const auto cos1 = std::cos(angle1);
    const auto sin1 = std::sin(angle1);
    const auto inner0 = vertex(center_x + cos0 * inner,
                               center_y + sin0 * inner);
    const auto outer0 = vertex(center_x + cos0 * outer,
                               center_y + sin0 * outer);
    const auto inner1 = vertex(center_x + cos1 * inner,
                               center_y + sin1 * inner);
    const auto outer1 = vertex(center_x + cos1 * outer,
                               center_y + sin1 * outer);
    const auto base = segment * 6;
    vertices[base + 0] = inner0;
    vertices[base + 1] = outer0;
    vertices[base + 2] = inner1;
    vertices[base + 3] = outer0;
    vertices[base + 4] = outer1;
    vertices[base + 5] = inner1;
  }
  renderer.draw_generic(std::span{vertices});
}

void draw_hand(auroreengine::gui_renderer &renderer,
               const ContainedLayout &layout, float center_x, float center_y,
               float angle, float length, float width, glm::vec4 color) {
  const auto dx = std::cos(angle);
  const auto dy = std::sin(angle);
  const auto px = -dy * width * 0.5F;
  const auto py = dx * width * 0.5F;
  const auto end_x = center_x + dx * length;
  const auto end_y = center_y + dy * length;
  const auto vertex = [&](float x, float y) {
    return auroreengine::simple_renderer::vertex_data{
        .position = {layout.x(x), layout.y(y)},
        .color = color,
        .tex_coords = {},
    };
  };
  const std::array vertices{
      vertex(center_x + px, center_y + py),
      vertex(center_x - px, center_y - py),
      vertex(end_x + px, end_y + py),
      vertex(center_x - px, center_y - py),
      vertex(end_x - px, end_y - py),
      vertex(end_x + px, end_y + py),
  };
  renderer.draw_generic(vertices);
}

void draw_clock_face(auroreengine::gui_renderer &renderer,
                     const ContainedLayout &layout,
                     const ClockHandAngles &angles, float center_x,
                     float center_y, float icon_radius, float x_offset,
                     float y_offset, float ring_width, float hand_width,
                     glm::vec4 color) {
  const auto cx = center_x + x_offset;
  const auto cy = center_y + y_offset;
  draw_ring(renderer, layout, cx, cy,
            icon_radius - ring_width * 0.5F - 0.6F,
            ring_width, color);
  draw_hand(renderer, layout, cx, cy, angles.hour_radians,
            icon_radius * 0.47F, hand_width, color);
  draw_hand(renderer, layout, cx, cy, angles.minute_radians,
            icon_radius * 0.69F, hand_width, color);
}

constexpr float battery_body_width = 23.0F;
constexpr float battery_body_height = 12.0F;
constexpr float battery_nub_width = 3.0F;
constexpr float battery_nub_height = 6.0F;
constexpr float battery_text_gap = 7.0F;
constexpr float wifi_pi = 3.14159265358979323846F;
constexpr float wifi_dot_radius = 1.45F;
constexpr std::array<float, 3> wifi_arc_radii{4.5F, 7.9F, 11.3F};
constexpr float wifi_arc_half_span = wifi_pi * 0.25F;
constexpr float wifi_stroke_reference = 1.125F;
constexpr float wifi_half_span_sine = 0.70710678F;
constexpr float wifi_indicator_width =
    2.0F * wifi_arc_radii.back() * wifi_half_span_sine + wifi_stroke_reference;
[[maybe_unused]] constexpr float wifi_indicator_height =
    wifi_arc_radii.back() + wifi_stroke_reference * 0.5F + wifi_dot_radius;
constexpr std::size_t kWifiDotSegments = 16;
constexpr std::size_t kWifiArcSegments = 16;
constexpr std::size_t kWifiCapSegments = 6;
constexpr float indicator_gap = 20.0F;

[[nodiscard]] auroreengine::simple_renderer::vertex_data
make_logical_vertex(const ContainedLayout &layout, float x, float y,
                    glm::vec4 color) noexcept {
  return {
      .position = {layout.x(x), layout.y(y)},
      .color = color,
      .tex_coords = {},
  };
}

void draw_filled_disc(auroreengine::gui_renderer &renderer,
                      const ContainedLayout &layout, float center_x,
                      float center_y, float radius, glm::vec4 color) {
  std::array<auroreengine::simple_renderer::vertex_data, kWifiDotSegments * 3>
      vertices{};
  constexpr auto full_turn = 2.0F * wifi_pi;
  const auto center = make_logical_vertex(layout, center_x, center_y, color);
  for (std::size_t segment = 0; segment < kWifiDotSegments; ++segment) {
    const auto angle0 = full_turn * static_cast<float>(segment) /
                        static_cast<float>(kWifiDotSegments);
    const auto angle1 = full_turn * static_cast<float>(segment + 1) /
                        static_cast<float>(kWifiDotSegments);
    const auto base = segment * 3;
    vertices[base + 0] = center;
    vertices[base + 1] = make_logical_vertex(
        layout, center_x + std::cos(angle0) * radius,
        center_y + std::sin(angle0) * radius, color);
    vertices[base + 2] = make_logical_vertex(
        layout, center_x + std::cos(angle1) * radius,
        center_y + std::sin(angle1) * radius, color);
  }
  renderer.draw_generic(std::span{vertices});
}

void draw_wifi_arc(auroreengine::gui_renderer &renderer,
                   const ContainedLayout &layout, float center_x,
                   float center_y, float radius, float stroke,
                   glm::vec4 color) {
  std::array<auroreengine::simple_renderer::vertex_data,
             kWifiArcSegments * 6 + kWifiCapSegments * 6>
      vertices{};
  constexpr auto angle_start = -0.5F * wifi_pi - wifi_arc_half_span;
  constexpr auto angle_end = -0.5F * wifi_pi + wifi_arc_half_span;
  const auto half_stroke = stroke * 0.5F;
  const auto inner = std::max(0.0F, radius - half_stroke);
  const auto outer = radius + half_stroke;

  for (std::size_t segment = 0; segment < kWifiArcSegments; ++segment) {
    const auto t0 =
        static_cast<float>(segment) / static_cast<float>(kWifiArcSegments);
    const auto t1 =
        static_cast<float>(segment + 1) / static_cast<float>(kWifiArcSegments);
    const auto angle0 = angle_start + (angle_end - angle_start) * t0;
    const auto angle1 = angle_start + (angle_end - angle_start) * t1;
    const auto cos0 = std::cos(angle0);
    const auto sin0 = std::sin(angle0);
    const auto cos1 = std::cos(angle1);
    const auto sin1 = std::sin(angle1);
    const auto inner0 = make_logical_vertex(layout, center_x + cos0 * inner,
                                            center_y + sin0 * inner, color);
    const auto outer0 = make_logical_vertex(layout, center_x + cos0 * outer,
                                            center_y + sin0 * outer, color);
    const auto inner1 = make_logical_vertex(layout, center_x + cos1 * inner,
                                            center_y + sin1 * inner, color);
    const auto outer1 = make_logical_vertex(layout, center_x + cos1 * outer,
                                            center_y + sin1 * outer, color);
    const auto base = segment * 6;
    vertices[base + 0] = inner0;
    vertices[base + 1] = outer0;
    vertices[base + 2] = inner1;
    vertices[base + 3] = outer0;
    vertices[base + 4] = outer1;
    vertices[base + 5] = inner1;
  }

  std::size_t cursor = kWifiArcSegments * 6;
  const auto append_cap = [&](float arc_angle, float sweep_start) {
    const auto cap_cx = center_x + std::cos(arc_angle) * radius;
    const auto cap_cy = center_y + std::sin(arc_angle) * radius;
    const auto cap_center = make_logical_vertex(layout, cap_cx, cap_cy, color);
    for (std::size_t segment = 0; segment < kWifiCapSegments; ++segment) {
      const auto a0 = sweep_start + wifi_pi * static_cast<float>(segment) /
                                        static_cast<float>(kWifiCapSegments);
      const auto a1 = sweep_start + wifi_pi *
                                        static_cast<float>(segment + 1) /
                                        static_cast<float>(kWifiCapSegments);
      vertices[cursor++] = cap_center;
      vertices[cursor++] = make_logical_vertex(
          layout, cap_cx + std::cos(a0) * half_stroke,
          cap_cy + std::sin(a0) * half_stroke, color);
      vertices[cursor++] = make_logical_vertex(
          layout, cap_cx + std::cos(a1) * half_stroke,
          cap_cy + std::sin(a1) * half_stroke, color);
    }
  };
  append_cap(angle_start, angle_start - wifi_pi);
  append_cap(angle_end, angle_end);
  renderer.draw_generic(std::span{vertices});
}

constexpr float status_icon_target_h = 22.0F;
constexpr float battery_slot_width = 28.5F;
constexpr float wifi_slot_width = 29.5F;
constexpr float plane_indicator_width = 22.0F;
constexpr float bluetooth_indicator_width = 15.0F;
constexpr float bluetooth_connected_indicator_width = 21.0F;

void draw_status_icon(auroreengine::gui_renderer &renderer,
                      const ContainedLayout &layout,
                      const auroreengine::texture &tex, float center_x,
                      float center_y, float draw_size, glm::vec4 tint) {
  const auto left = center_x - draw_size * 0.5F;
  const auto top = center_y - draw_size * 0.5F;
  const auto extent = draw_size * layout.scale / layout.framebuffer_height;
  renderer.draw_image(tex, layout.x(left), layout.y(top), extent, extent, tint);
}

void draw_battery_indicator(auroreengine::gui_renderer &renderer,
                            const ContainedLayout &layout,
                            const StatusBarIcons &icons, float left_x,
                            float center_y, int percent, bool charging,
                            float opacity) {
  const auroreengine::texture *tex = nullptr;
  glm::vec4 tint{1.0F, 1.0F, 1.0F, 0.94F * opacity};
  float draw_size = status_icon_target_h * (128.0F / 64.0F) * 0.86F;
  float y_offset = 0.0F;
  if (charging) {
    if (percent >= 100) {
      tex = icons.battery_charging_2;
      draw_size = status_icon_target_h * (128.0F / 91.0F);
      // Center the plug+battery glyph vertically (PNG visual center at y=77/128).
      y_offset = -((77.0F - 63.5F) / 128.0F) * draw_size;
    } else {
      tex = icons.battery_charging;
    }
    tint = glm::vec4{0.52F, 1.0F, 0.62F, 0.95F * opacity};
  } else if (percent < 10) {
    tex = icons.battery_exclamation;
    draw_size = status_icon_target_h * (128.0F / 91.0F);
    y_offset = -((77.0F - 63.5F) / 128.0F) * draw_size;
    tint = glm::vec4{1.0F, 0.42F, 0.42F, 0.95F * opacity};
  } else if (percent <= 25) {
    tex = icons.battery_1;
  } else if (percent <= 50) {
    tex = icons.battery_2;
  } else if (percent <= 75) {
    tex = icons.battery_3;
  } else {
    tex = icons.battery_4;
  }
  if (tex != nullptr && tex->loaded) {
    draw_status_icon(renderer, layout, *tex, left_x + battery_slot_width * 0.5F,
                     center_y + y_offset, draw_size, tint);
    return;
  }
  // Vector fallback when textures are not bound (e.g. headless tests).
  const auto stroke = logical_hairline(layout) * 1.8F;
  const auto alpha = 0.92F * opacity;
  const auto outline = glm::vec4{1.0F, 1.0F, 1.0F, alpha};
  const auto top = center_y - battery_body_height * 0.5F;
  draw_logical_rect(renderer, layout, left_x, top, battery_body_width, stroke, outline);
  draw_logical_rect(renderer, layout, left_x, top + battery_body_height - stroke, battery_body_width, stroke, outline);
  draw_logical_rect(renderer, layout, left_x, top, stroke, battery_body_height, outline);
  draw_logical_rect(renderer, layout, left_x + battery_body_width - stroke, top, stroke, battery_body_height, outline);
  draw_logical_rect(renderer, layout, left_x + battery_body_width, center_y - battery_nub_height * 0.5F, battery_nub_width, battery_nub_height, outline);
}

void draw_wifi_indicator(auroreengine::gui_renderer &renderer,
                         const ContainedLayout &layout,
                         const StatusBarIcons &icons, float left_x,
                         float center_y, int signal, bool no_internet,
                         float opacity) {
  const auroreengine::texture *tex = nullptr;
  glm::vec4 tint{1.0F, 1.0F, 1.0F, 0.94F * opacity};
  float draw_size = status_icon_target_h * (128.0F / 76.0F);
  if (signal <= 0) {
    tex = icons.wifi_0;
    tint = glm::vec4{1.0F, 1.0F, 1.0F, 0.35F * opacity};
  } else if (no_internet) {
    tex = icons.wifi_off;
    draw_size = status_icon_target_h * (128.0F / 108.0F);
    tint = glm::vec4{1.0F, 1.0F, 1.0F, 0.88F * opacity};
  } else if (signal >= 75) {
    tex = icons.wifi;
  } else if (signal >= 50) {
    tex = icons.wifi_2;
  } else if (signal >= 25) {
    tex = icons.wifi_1;
  } else {
    tex = icons.wifi_0;
  }
  if (tex != nullptr && tex->loaded) {
    draw_status_icon(renderer, layout, *tex, left_x + wifi_slot_width * 0.5F,
                     center_y, draw_size, tint);
    return;
  }
  const auto stroke = logical_hairline(layout) * 1.8F;
  const auto lit = signal <= 0    ? 0
                   : signal >= 70 ? 3
                   : signal >= 40 ? 2
                                  : 1;
  const auto lit_color = glm::vec4{1.0F, 1.0F, 1.0F, 0.94F * opacity};
  const auto dim_color = glm::vec4{1.0F, 1.0F, 1.0F, 0.28F * opacity};
  const auto total_height = wifi_arc_radii.back() + stroke * 0.5F + wifi_dot_radius;
  const auto center_x = left_x + wifi_indicator_width * 0.5F;
  const auto origin_y = center_y + total_height * 0.5F - wifi_dot_radius;
  draw_filled_disc(renderer, layout, center_x, origin_y, wifi_dot_radius, lit > 0 ? lit_color : dim_color);
  for (std::size_t index = 0; index < wifi_arc_radii.size(); ++index) {
    draw_wifi_arc(renderer, layout, center_x, origin_y, wifi_arc_radii[index],
                  stroke, static_cast<int>(index) < lit ? lit_color : dim_color);
  }
}

void draw_segment(auroreengine::gui_renderer &renderer,
                  const ContainedLayout &layout, float x0, float y0, float x1,
                  float y1, float width, glm::vec4 color) {
  const auto dx = x1 - x0;
  const auto dy = y1 - y0;
  draw_hand(renderer, layout, x0, y0, std::atan2(dy, dx),
            std::hypot(dx, dy), width, color);
}

void draw_bluetooth_indicator(auroreengine::gui_renderer &renderer,
                              const ContainedLayout &layout,
                              const StatusBarIcons &icons, float left_x,
                              float slot_width, float center_y, bool connected,
                              float opacity) {
  const auto color = glm::vec4{1.0F, 1.0F, 1.0F, 0.94F * opacity};
  const auto *tex = connected ? icons.bluetooth_connected : icons.bluetooth;
  if (tex != nullptr && tex->loaded) {
    const float draw_size = status_icon_target_h * (128.0F / 96.0F);
    draw_status_icon(renderer, layout, *tex, left_x + slot_width * 0.5F,
                     center_y, draw_size, color);
    return;
  }
  const auto stroke = logical_hairline(layout) * 1.8F;
  const auto cx = left_x + slot_width * 0.5F;
  const auto top = center_y - 7.0F;
  const auto bottom = center_y + 7.0F;
  draw_logical_rect(renderer, layout, cx - stroke * 0.5F, top, stroke,
                    bottom - top, color);
  draw_segment(renderer, layout, cx - 4.5F, center_y - 3.5F, cx + 4.5F,
               center_y + 3.5F, stroke, color);
  draw_segment(renderer, layout, cx - 4.5F, center_y + 3.5F, cx + 4.5F,
               center_y - 3.5F, stroke, color);
  draw_segment(renderer, layout, cx + 4.5F, center_y - 3.5F, cx, top, stroke,
               color);
  draw_segment(renderer, layout, cx + 4.5F, center_y + 3.5F, cx, bottom,
               stroke, color);
}

void draw_aeroplane_indicator(auroreengine::gui_renderer &renderer,
                              const ContainedLayout &layout,
                              const StatusBarIcons &icons, float left_x,
                              float slot_width, float center_y,
                              float opacity) {
  const auto color = glm::vec4{1.0F, 1.0F, 1.0F, 0.94F * opacity};
  if (icons.aeroplane != nullptr && icons.aeroplane->loaded) {
    const float draw_size = status_icon_target_h * (128.0F / 108.0F);
    draw_status_icon(renderer, layout, *icons.aeroplane,
                     left_x + slot_width * 0.5F, center_y,
                     draw_size, color);
    return;
  }
  const auto stroke = logical_hairline(layout) * 1.8F;
  const auto cx = left_x + slot_width * 0.5F;
  draw_segment(renderer, layout, cx - 5.0F, center_y + 5.0F,
               cx + 5.0F, center_y - 5.0F, stroke, color);
  draw_segment(renderer, layout, cx - 3.5F, center_y - 1.0F,
               cx + 1.0F, center_y + 3.5F, stroke, color);
  draw_segment(renderer, layout, cx + 2.0F, center_y - 4.5F,
               cx + 4.5F, center_y - 2.0F, stroke, color);
}

} // namespace

void StatusBarRenderer::render(auroreengine::gui_renderer &renderer,
                               const StatusBarTime &time, float opacity,
                               std::string_view label,
                               const StatusBarStatus &status) const {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity <= 0.001F || !valid_status_bar_time(time)) {
    return;
  }

  std::array<char, 16> text_buffer{};
  const auto text_size = format_status_bar_time(time, text_buffer);
  if (text_size == 0) {
    return;
  }
  const std::string_view time_text =
      label.empty() ? std::string_view(text_buffer.data(), text_size) : label;
  const auto layout = make_layout(renderer);
  const auto scale_guard = std::max(1.0F, layout.scale);

  // Submenu labels sit at 30 logical px without the shared text scale
  // (main_menu.cpp), so the status text uses 26 and must not multiply
  // config::CONFIG.textScale in again: the canvas callers already apply it.
  const auto text_px = 26.0F * layout.scale;
  const auto text_w_logical =
      renderer.text_width_pixels(time_text, text_px) / scale_guard;
  const auto metrics =
      renderer.text_line_metrics_pixels(text_px) / scale_guard;
  const auto logical_text_h = metrics.x + metrics.y;
  const auto frame_height = logical_text_h + 16.0F;
  const auto icon_radius = 11.0F;
  const auto icon_scale = icon_radius / StatusBarMetrics::icon_radius;
  // A glass capsule floating in from the top-right corner of the screen.
  const auto visible_top = layout.offset_y < 0.0F ? -layout.offset_y / layout.scale : 0.0F;
  const auto frame_y = StatusBarMetrics::frame_top(visible_top);
  const auto icon_center_y = frame_y + frame_height * 0.5F;
  const auto end_pad = std::max(StatusBarMetrics::frame_radius(frame_height) * 0.85F, 22.0F);

  const auto battery_present = status.battery_percent >= 0;
  // In aeroplane mode the radios are blocked: show one plane at the first
  // radio slot instead of Wi-Fi or Bluetooth.
  const auto plane_present = status.aeroplane_on;
  const auto wifi_present = !plane_present && status.wifi_signal >= 0;
  const auto bluetooth_present = !plane_present && status.bluetooth_on;
  const auto bluetooth_width = status.bluetooth_connected
                                   ? bluetooth_connected_indicator_width
                                   : bluetooth_indicator_width;
  const auto clock_gap = 10.0F;
  const auto clock_width = text_w_logical +
      (status.analog_clock ? (clock_gap + icon_radius * 2.0F) : 0.0F);

  enum class DrawSlot { battery, wifi, bluetooth, plane, clock };
  struct SlotLayout {
    DrawSlot kind;
    float width{};
    float left{};
  };
  std::array<SlotLayout, 4> slots{};
  std::size_t slot_count = 0;
  bool plane_added = false;
  for (const auto element : status.element_order) {
    switch (element) {
      case StatusBarElement::battery:
        if (battery_present) {
          slots[slot_count++] = {DrawSlot::battery, battery_slot_width, 0.0F};
        }
        break;
      case StatusBarElement::wifi:
        if (plane_present && !plane_added) {
          slots[slot_count++] = {DrawSlot::plane, plane_indicator_width, 0.0F};
          plane_added = true;
        } else if (wifi_present) {
          slots[slot_count++] = {DrawSlot::wifi, wifi_slot_width, 0.0F};
        }
        break;
      case StatusBarElement::bluetooth:
        if (plane_present && !plane_added) {
          slots[slot_count++] = {DrawSlot::plane, plane_indicator_width, 0.0F};
          plane_added = true;
        } else if (bluetooth_present) {
          slots[slot_count++] = {DrawSlot::bluetooth, bluetooth_width, 0.0F};
        }
        break;
      case StatusBarElement::clock:
        slots[slot_count++] = {DrawSlot::clock, clock_width, 0.0F};
        break;
    }
  }

  float content_w = 0.0F;
  for (std::size_t i = 0; i < slot_count; ++i) {
    if (i > 0) content_w += indicator_gap;
    content_w += slots[i].width;
  }

  const auto frame_x = std::max(
      0.0F, StatusBarMetrics::frame_right() - end_pad - content_w - end_pad);
  const auto frame_w = StatusBarMetrics::frame_right() - frame_x;
  float cursor_x = frame_x + end_pad;
  for (std::size_t i = 0; i < slot_count; ++i) {
    slots[i].left = cursor_x;
    cursor_x += slots[i].width + indicator_gap;
  }

  openxmb::xmb::glass::draw_glass_px<auroreengine::glass_panel_params, auroreengine::simple_params>(
      renderer, layout.offset_x + frame_x * layout.scale, layout.offset_y + frame_y * layout.scale,
      frame_w * layout.scale, frame_height * layout.scale, openxmb::xmb::glass::variant::regular,
      StatusBarMetrics::frame_radius(frame_height) * layout.scale, opacity, layout.scale);

  const auto baseline_logical =
      frame_y + (frame_height - logical_text_h) * 0.5F + metrics.x;
  const auto text_px_y = layout.offset_y + baseline_logical * layout.scale;
  const auto draw_label = [&](std::string_view text, float logical_left) {
    const auto x = layout.offset_x + logical_left * layout.scale;
    renderer.draw_text_pixels(text, x, text_px_y + layout.scale, text_px,
                              {0.0F, 0.0F, 0.0F, 0.5F * opacity}, 0.7F);
    renderer.draw_text_pixels(text, x, text_px_y, text_px,
                              {1.0F, 1.0F, 1.0F, 1.0F * opacity}, 0.7F);
  };

  for (std::size_t i = 0; i < slot_count; ++i) {
    const auto &slot = slots[i];
    switch (slot.kind) {
      case DrawSlot::battery:
        draw_battery_indicator(renderer, layout, icons_, slot.left, icon_center_y,
                               status.battery_percent, status.charging, opacity);
        break;
      case DrawSlot::wifi:
        draw_wifi_indicator(renderer, layout, icons_, slot.left, icon_center_y,
                            status.wifi_signal, status.wifi_no_internet, opacity);
        break;
      case DrawSlot::bluetooth:
        draw_bluetooth_indicator(renderer, layout, icons_, slot.left,
                                 bluetooth_width, icon_center_y,
                                 status.bluetooth_connected, opacity);
        break;
      case DrawSlot::plane:
        draw_aeroplane_indicator(renderer, layout, icons_, slot.left,
                                 plane_indicator_width, icon_center_y, opacity);
        break;
      case DrawSlot::clock:
        draw_label(time_text, slot.left);
        if (status.analog_clock) {
          const auto icon_center_x = slot.left + text_w_logical + clock_gap + icon_radius;
          const auto angles = resolve_clock_hand_angles(time);
          draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                          icon_radius, 0.0F, 2.0F * icon_scale, 2.6F * icon_scale,
                          4.0F * icon_scale, {0.0F, 0.0F, 0.0F, 0.55F * opacity});
          draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                          icon_radius, 0.0F, 0.0F, 5.2F * icon_scale,
                          6.2F * icon_scale, {1.0F, 1.0F, 1.0F, 0.14F * opacity});
          draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                          icon_radius, 0.0F, 0.0F, 2.6F * icon_scale,
                          4.0F * icon_scale, {1.0F, 1.0F, 1.0F, 0.96F * opacity});
        }
        break;
    }
  }
}

bool StatusBarRenderer::render_local(
    auroreengine::gui_renderer &renderer,
    std::chrono::system_clock::time_point time, float opacity) const {
  time+=std::chrono::seconds(static_cast<int>(config::CONFIG.number("time.offset",0)*3600));
  const auto local = resolve_local_status_bar_time(time);
  if (!local.has_value()) {
    return false;
  }
  const auto civil=std::chrono::system_clock::to_time_t(time);std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm,&civil);
#else
  localtime_r(&civil,&tm);
#endif
  const auto date=config::CONFIG.preference("date.format","DD/MM/YYYY");
  std::string format=date=="YYYY-MM-DD"?"%Y-%m-%d":date=="MM/DD/YYYY"?"%m/%d/%Y":"%d/%m/%Y";
  format+=config::CONFIG.preference("time.format","24-hour")=="12-hour"?" %I:%M":" %H:%M";
  if(config::CONFIG.enabled("time.seconds",false))format+=":%S";
  if(config::CONFIG.preference("time.format","24-hour")=="12-hour")format+=" %p";
  std::array<char,64> formatted{};std::strftime(formatted.data(),formatted.size(),format.c_str(),&tm);
  auto status = polled_status_bar_status();
  status.analog_clock = config::CONFIG.enabled("status.analog.clock", false);
  status.element_order = openxmb::xmb::parse_status_bar_order(
      config::CONFIG.preference("status.bar.order"));
  render(renderer, *local, opacity, formatted.data(), status);
  return true;
}

} // namespace openxmb::xmb
