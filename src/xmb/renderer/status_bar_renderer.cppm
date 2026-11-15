module;
#include "openxmb/xmb/glass_draw.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <ctime>

#include "openxmb/xmb/status_bar.hpp"

export module openxmb.xmb.status_bar_renderer;

import auroreengine;
import glm;
import openxmb.config;

export namespace openxmb::xmb {

class StatusBarRenderer {
public:
  void render(auroreengine::gui_renderer &renderer, const StatusBarTime &time,
              float opacity = 1.0F, std::string_view label = {},
              const StatusBarStatus &status = {}) const;

  // Convenience path for production. Deterministic tests and captures should
  // resolve their fixed civil time once and use the overload above.
  [[nodiscard]] bool
  render_local(auroreengine::gui_renderer &renderer,
               std::chrono::system_clock::time_point time,
               float opacity = 1.0F) const;
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

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
// means associated, a down link keeps the bars visible but dim, and no
// wireless interface at all hides them.
[[nodiscard]] int read_wifi_signal() {
  std::error_code error;
  const std::filesystem::path net_root{"/sys/class/net"};
  std::string connected;
  bool wireless_present = false;
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
      connected = name;
      break;
    }
  }
  if (!wireless_present) {
    return -1;
  }
  if (connected.empty()) {
    return 0;
  }
  const auto level = wireless_level_dbm(connected);
  // A cfg80211 driver that exports no legacy level is still associated, so
  // show three bars rather than a false weak link.
  return level.has_value() ? wireless_signal_from_dbm(*level) : 100;
}

[[nodiscard]] StatusBarStatus read_status_bar_status() {
  StatusBarStatus status{};
  const auto supply = battery_supply_path();
  if (!supply.empty()) {
    status.battery_percent =
        parse_status_percent(read_sysfs_text(supply / "capacity"));
    const auto state = read_sysfs_text(supply / "status");
    status.charging = state == "Full" || state.starts_with("Charg");
  }
  status.wifi_signal = read_wifi_signal();
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

void draw_u_frame(auroreengine::gui_renderer &renderer,
                  const ContainedLayout &layout, float opacity) {
  constexpr std::array glow_alpha{0.13F, 0.095F, 0.065F, 0.035F, 0.02F};
  for (std::size_t index = 0; index < glow_alpha.size(); ++index) {
    const auto inset = static_cast<float>(index + 1);
    const auto alpha = glow_alpha[index] * opacity;
    const auto x = StatusBarMetrics::frame_x + inset;
    const auto y = StatusBarMetrics::frame_y + inset;
    const auto width = StatusBarMetrics::frame_width + 4.0F - inset -
                       StatusBarMetrics::frame_corner_radius;
    const auto height = StatusBarMetrics::frame_height - inset * 2.0F;
    draw_logical_rect(renderer, layout,
                      x + StatusBarMetrics::frame_corner_radius, y, width,
                      1.0F, {1.0F, 1.0F, 1.0F, alpha});
    draw_logical_rect(renderer, layout, x, y + StatusBarMetrics::frame_corner_radius,
                      1.0F,
                      height - StatusBarMetrics::frame_corner_radius * 2.0F,
                      {1.0F, 1.0F, 1.0F, alpha});
    draw_logical_rect(renderer, layout,
                      x + StatusBarMetrics::frame_corner_radius,
                      y + height - 1.0F, width, 1.0F,
                      {1.0F, 1.0F, 1.0F, alpha});
  }

  constexpr auto border_alpha = 0.175F;
  constexpr auto horizontal_rule_width =
      StatusBarMetrics::frame_width + 4.0F -
      StatusBarMetrics::frame_corner_radius;
  draw_logical_rect(renderer, layout,
                    StatusBarMetrics::frame_x +
                        StatusBarMetrics::frame_corner_radius,
                    StatusBarMetrics::frame_y, horizontal_rule_width, 1.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
  draw_logical_rect(renderer, layout, StatusBarMetrics::frame_x,
                    StatusBarMetrics::frame_y +
                        StatusBarMetrics::frame_corner_radius,
                    1.0F,
                    StatusBarMetrics::frame_height -
                        StatusBarMetrics::frame_corner_radius * 2.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
  draw_logical_rect(renderer, layout,
                    StatusBarMetrics::frame_x +
                        StatusBarMetrics::frame_corner_radius,
                    StatusBarMetrics::frame_y +
                        StatusBarMetrics::frame_height - 1.0F,
                    horizontal_rule_width, 1.0F,
                    {1.0F, 1.0F, 1.0F, border_alpha * opacity});
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
constexpr float indicator_gap = 12.0F;

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

// Outline, nub and proportional fill. Charging lightens the fill towards green
// so the state reads without a second glyph.
void draw_battery_indicator(auroreengine::gui_renderer &renderer,
                            const ContainedLayout &layout, float left_x,
                            float center_y, int percent, bool charging,
                            float opacity) {
  const auto stroke = logical_hairline(layout);
  const auto alpha = 0.92F * opacity;
  const auto outline = glm::vec4{1.0F, 1.0F, 1.0F, alpha};
  const auto top = center_y - battery_body_height * 0.5F;
  draw_logical_rect(renderer, layout, left_x, top, battery_body_width, stroke,
                    outline);
  draw_logical_rect(renderer, layout, left_x,
                    top + battery_body_height - stroke, battery_body_width,
                    stroke, outline);
  draw_logical_rect(renderer, layout, left_x, top, stroke, battery_body_height,
                    outline);
  draw_logical_rect(renderer, layout, left_x + battery_body_width - stroke,
                    top, stroke, battery_body_height, outline);
  draw_logical_rect(renderer, layout, left_x + battery_body_width,
                    center_y - battery_nub_height * 0.5F, battery_nub_width,
                    battery_nub_height, outline);

  const auto inset = stroke * 1.75F;
  const auto inner_width = battery_body_width - inset * 2.0F;
  const auto inner_height = battery_body_height - inset * 2.0F;
  const auto level = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0F;
  if (level <= 0.0F) {
    return;
  }
  const auto fill = charging ? glm::vec4{0.52F, 1.0F, 0.62F, alpha}
                             : glm::vec4{1.0F, 1.0F, 1.0F, alpha};
  draw_logical_rect(renderer, layout, left_x + inset, top + inset,
                    std::max(stroke, inner_width * level), inner_height,
                    fill);
}

// Bottom dot and three concentric arcs, lit from the inside out in proportion
// to link quality; a present but unassociated interface keeps them dim instead
// of hiding the indicator.
void draw_wifi_indicator(auroreengine::gui_renderer &renderer,
                         const ContainedLayout &layout, float left_x,
                         float center_y, int signal, float opacity) {
  const auto stroke = logical_hairline(layout);
  const auto lit = signal <= 0    ? 0
                   : signal >= 70 ? 3
                   : signal >= 40 ? 2
                                  : 1;
  const auto lit_color = glm::vec4{1.0F, 1.0F, 1.0F, 0.92F * opacity};
  const auto dim_color = glm::vec4{1.0F, 1.0F, 1.0F, 0.28F * opacity};

  const auto total_height =
      wifi_arc_radii.back() + stroke * 0.5F + wifi_dot_radius;
  const auto center_x = left_x + wifi_indicator_width * 0.5F;
  const auto origin_y = center_y + total_height * 0.5F - wifi_dot_radius;

  draw_filled_disc(renderer, layout, center_x, origin_y, wifi_dot_radius,
                   lit > 0 ? lit_color : dim_color);
  for (std::size_t index = 0; index < wifi_arc_radii.size(); ++index) {
    const auto color =
        static_cast<int>(index) < lit ? lit_color : dim_color;
    draw_wifi_arc(renderer, layout, center_x, origin_y, wifi_arc_radii[index],
                  stroke, color);
  }
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
  const auto icon_radius = 10.5F;
  const auto icon_scale = icon_radius / StatusBarMetrics::icon_radius;
  // A glass capsule floating in from the top-right corner of the screen.
  const auto visible_top = layout.offset_y < 0.0F ? -layout.offset_y / layout.scale : 0.0F;
  const auto frame_y = StatusBarMetrics::frame_top(visible_top);
  const auto icon_center_y = frame_y + frame_height * 0.5F;
  const auto end_pad = StatusBarMetrics::frame_radius(frame_height) * 0.8F;

  const auto icon_center_x =
      StatusBarMetrics::frame_right() - end_pad - icon_radius;
  const auto text_right_x = icon_center_x - icon_radius - 10.0F;
  const auto text_left_x = text_right_x - text_w_logical;

  // The indicators grow the glass frame leftwards from the date/time; a value
  // that is unknown is dropped from the row instead of leaving a gap.
  std::string battery_text;
  auto battery_text_width = 0.0F;
  if (status.battery_percent >= 0) {
    battery_text = std::to_string(std::clamp(status.battery_percent, 0, 100));
    battery_text += status.charging ? "%+" : "%";
    battery_text_width =
        renderer.text_width_pixels(battery_text, text_px) / scale_guard;
  }
  const auto battery_present = !battery_text.empty();
  const auto wifi_present = status.wifi_signal >= 0;

  auto cursor_x = text_left_x;
  auto wifi_left = cursor_x;
  if (wifi_present) {
    wifi_left = cursor_x - indicator_gap - wifi_indicator_width;
    cursor_x = wifi_left;
  }
  auto battery_left = cursor_x;
  auto battery_text_left = cursor_x;
  if (battery_present) {
    battery_left = cursor_x - indicator_gap - battery_body_width -
                   battery_nub_width - battery_text_gap - battery_text_width;
    battery_text_left = battery_left + battery_body_width +
                        battery_nub_width + battery_text_gap;
    cursor_x = battery_left;
  }

  const auto frame_x = std::max(0.0F, cursor_x - end_pad);
  const auto frame_w = StatusBarMetrics::frame_right() - frame_x;
  openxmb::xmb::glass::draw_glass_px<auroreengine::glass_panel_params, auroreengine::simple_params>(
      renderer, layout.offset_x + frame_x * layout.scale, layout.offset_y + frame_y * layout.scale,
      frame_w * layout.scale, frame_height * layout.scale, openxmb::xmb::glass::variant::regular,
      StatusBarMetrics::frame_radius(frame_height) * layout.scale, opacity, layout.scale);

  if (wifi_present) {
    draw_wifi_indicator(renderer, layout, wifi_left, icon_center_y,
                        status.wifi_signal, opacity);
  }
  if (battery_present) {
    draw_battery_indicator(renderer, layout, battery_left, icon_center_y,
                           status.battery_percent, status.charging, opacity);
  }

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
  if (battery_present) {
    draw_label(battery_text, battery_text_left);
  }
  draw_label(time_text, text_left_x);

  const auto angles = resolve_clock_hand_angles(time);
  draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                  icon_radius, 0.0F, 2.0F * icon_scale, 2.6F * icon_scale,
                  4.0F * icon_scale, {0.0F, 0.0F, 0.0F, 0.55F * opacity});
  draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                  icon_radius, 0.0F, 0.0F, 5.2F * icon_scale,
                  6.2F * icon_scale, {1.0F, 1.0F, 1.0F, 0.14F * opacity});
  draw_clock_face(renderer, layout, angles, icon_center_x, icon_center_y,
                  icon_radius, 0.0F, 0.0F, 2.6F * icon_scale, 4.0F * icon_scale,
                  {1.0F, 1.0F, 1.0F, 0.96F * opacity});
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
  render(renderer, *local, opacity, formatted.data(),
         polled_status_bar_status());
  return true;
}

} // namespace openxmb::xmb
