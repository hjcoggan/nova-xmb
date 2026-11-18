#pragma once

// The console's one glass material: its variants, shape and colour tokens,
// and the small amount of maths every surface shares. Design units are the
// 1920x1080 authoring space of app::xmb_canvas. (The full material spec is a
// local-only note under docs/superpowers/, which is not tracked in git.)

#include "openxmb/xmb/ui_tokens.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace openxmb::xmb::glass {

enum class variant { regular, clear, focus };

// How a variant treats the scene behind it. The engine's panel shader takes
// these values as they are.
struct material {
  float refraction_px;    // how far the edge bends the backdrop
  float blur_lod;         // mip level of the backdrop that is sampled
  float tint;             // white added to the body (focus: the lift)
  float dim_strength;     // 1 = darken bright backdrops to the contrast cap
  bool samples_backdrop;  // false: a lit region, no body of its own
};

[[nodiscard]] constexpr material material_for(variant v) noexcept {
  switch (v) {
    case variant::regular: return {10.0F, 3.5F, 0.012F, 1.0F, true};
    case variant::clear: return {8.0F, 3.0F, 0.006F, 1.0F, true};
    case variant::focus: return {0.0F, 0.0F, 0.05F, 0.0F, false};
  }
  return {10.0F, 3.5F, 0.012F, 1.0F, true};
}

// Shape tokens come from the one ui scale; these names stay for callers.
inline constexpr float card_radius = ui::px(ui::radius::xl);
inline constexpr float card_padding = ui::px(ui::space::xl);
inline constexpr float grid = ui::px(ui::space::xs);
// White text holds 7:1 against a body no brighter than this (4.5:1, the
// floor, looked washed out over bright paused games on the device).
inline constexpr float max_body_luminance = 0.10F;
// Focus and slider fill fallback (overridden at runtime by the active theme).
inline constexpr std::array<float, 3> accent_srgb{10.0F, 132.0F, 255.0F};
inline constexpr float text_primary = 1.0F;
inline constexpr float text_secondary = 0.70F;
inline constexpr float text_disabled = 0.40F;
inline constexpr float focus_slide_ms = 150.0F;
inline constexpr float panel_motion_ms = 200.0F;
inline constexpr float panel_start_scale = 0.96F;

// One sRGB byte as linear light (the render target is linear).
[[nodiscard]] inline float srgb_to_linear(float byte) noexcept {
  const float v = byte / 255.0F;
  return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
}

namespace detail {
inline std::array<float, 3> active_base_linear{};
inline std::array<float, 3> active_accent_linear_rgb{};
inline bool theme_override_active = false;
}  // namespace detail

inline void set_theme_colors(std::array<float, 3> base_srgb_unit,
                             std::array<float, 3> accent_srgb_unit) noexcept {
  detail::active_base_linear = {
      srgb_to_linear(std::clamp(base_srgb_unit[0], 0.0F, 1.0F) * 255.0F),
      srgb_to_linear(std::clamp(base_srgb_unit[1], 0.0F, 1.0F) * 255.0F),
      srgb_to_linear(std::clamp(base_srgb_unit[2], 0.0F, 1.0F) * 255.0F),
  };
  detail::active_accent_linear_rgb = {
      srgb_to_linear(std::clamp(accent_srgb_unit[0], 0.0F, 1.0F) * 255.0F),
      srgb_to_linear(std::clamp(accent_srgb_unit[1], 0.0F, 1.0F) * 255.0F),
      srgb_to_linear(std::clamp(accent_srgb_unit[2], 0.0F, 1.0F) * 255.0F),
  };
  detail::theme_override_active = true;
}

inline void clear_theme_colors() noexcept {
  detail::theme_override_active = false;
}

[[nodiscard]] inline std::array<float, 3> active_accent_linear() noexcept {
  if (detail::theme_override_active) {
    return detail::active_accent_linear_rgb;
  }
  return {srgb_to_linear(accent_srgb[0]), srgb_to_linear(accent_srgb[1]),
          srgb_to_linear(accent_srgb[2])};
}

[[nodiscard]] inline std::array<float, 4> active_theme_tint_linear() noexcept {
  if (!detail::theme_override_active) {
    return {0.0F, 0.0F, 0.0F, 0.0F};
  }
  return {detail::active_base_linear[0], detail::active_base_linear[1],
          detail::active_base_linear[2], 0.85F};
}

// The rim light of a variant, linear RGBA: one white rim for every pane of
// glass, the accent for focus.
[[nodiscard]] inline std::array<float, 4> rim_for(variant v) noexcept {
  if (v == variant::focus)
    return {srgb_to_linear(accent_srgb[0]), srgb_to_linear(accent_srgb[1]), srgb_to_linear(accent_srgb[2]), 0.9F};
  return {1.0F, 1.0F, 1.0F, 0.55F};
}

[[nodiscard]] inline std::array<float, 4> draw_rim_for(variant v) noexcept {
  if (v == variant::focus && detail::theme_override_active) {
    const auto accent = detail::active_accent_linear_rgb;
    return {
        0.75F + 0.25F * accent[0],
        0.75F + 0.25F * accent[1],
        0.75F + 0.25F * accent[2],
        0.72F,
    };
  }
  return rim_for(v);
}

[[nodiscard]] constexpr float concentric_radius(float outer, float inset) noexcept {
  return ui::concentric(outer, inset);
}

[[nodiscard]] constexpr float clamp_radius(float radius, float width, float height) noexcept {
  return std::clamp(radius, 0.0F, std::max(0.0F, std::min(width, height) * 0.5F));
}

// The share of a backdrop of this relative luminance to darken so that
// backdrop*(1-d) + tint stays at max_body_luminance. The panel shader uses
// the same formula.
[[nodiscard]] constexpr float dim_for_contrast(float backdrop_luminance, float tint) noexcept {
  if (backdrop_luminance <= 0.0F) return 0.0F;
  return std::clamp(1.0F - (max_body_luminance - tint) / backdrop_luminance, 0.0F, 1.0F);
}

// WCAG contrast ratio between two relative luminances.
[[nodiscard]] constexpr float contrast_ratio(float l1, float l2) noexcept {
  return (std::max(l1, l2) + 0.05F) / (std::min(l1, l2) + 0.05F);
}

struct rect {
  float x, y, w, h;
};

[[nodiscard]] constexpr float ease_out_cubic(float t) noexcept {
  t = std::clamp(t, 0.0F, 1.0F);
  const float u = 1.0F - t;
  return 1.0F - u * u * u;
}

// Where the focus capsule is, elapsed_ms after focus moved from `from` to `to`.
[[nodiscard]] constexpr rect focus_rect_at(rect from, rect to, float elapsed_ms) noexcept {
  const float k = ease_out_cubic(elapsed_ms / focus_slide_ms);
  return {from.x + (to.x - from.x) * k, from.y + (to.y - from.y) * k,
          from.w + (to.w - from.w) * k, from.h + (to.h - from.h) * k};
}

// The focus capsule of one list: where it is now, sliding to the focused
// row. Times are milliseconds on any steady clock.
struct focus_tracker {
  rect from{}, to{};
  float started_ms = 0;
  bool placed = false;

  void move_to(rect target, float now_ms) noexcept {
    if (!placed) {
      from = to = target;
      started_ms = now_ms;
      placed = true;
      return;
    }
    if (target.x == to.x && target.y == to.y && target.w == to.w && target.h == to.h) return;
    from = at(now_ms);
    to = target;
    started_ms = now_ms;
  }

  [[nodiscard]] rect at(float now_ms) const noexcept { return focus_rect_at(from, to, now_ms - started_ms); }
};

struct panel_pose {
  float alpha;
  float scale;
};

[[nodiscard]] constexpr panel_pose panel_in(float elapsed_ms) noexcept {
  const float k = ease_out_cubic(elapsed_ms / panel_motion_ms);
  return {k, panel_start_scale + (1.0F - panel_start_scale) * k};
}

[[nodiscard]] constexpr panel_pose panel_out(float elapsed_ms) noexcept {
  const auto in = panel_in(panel_motion_ms - std::clamp(elapsed_ms, 0.0F, panel_motion_ms));
  return in;
}

}  // namespace openxmb::xmb::glass
