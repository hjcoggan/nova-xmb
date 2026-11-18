#pragma once

// The console's only design-token scales. Every surface uses these values;
// no screen picks its own padding, corner radius or type size.
//
// Authoring units are the 1920x1080 design space of app::xmb_canvas, the same
// space glass.hpp uses. The renderer contains that rectangle in the panel.

#include <algorithm>

namespace openxmb::xmb::ui {

// 4-point spacing scale. Use the enum, never a literal, for container padding.
enum class space {
  xxs = 4,
  xs = 8,
  sm = 12,
  md = 16,
  lg = 24,
  xl = 32,
};

[[nodiscard]] constexpr float px(space value) noexcept {
  return static_cast<float>(static_cast<int>(value));
}

// Concentric corner scale. The steps are chosen so that subtracting the
// spacing scale gives the next radius inwards: 36-8=28, 28-8=20, 20-8=12,
// 12-4=8. An inner surface inset by one spacing step shares a centre with its
// container, which is the whole point of concentric corners.
enum class radius {
  xs = 8,
  sm = 12,
  md = 20,
  lg = 28,
  xl = 36,
};

[[nodiscard]] constexpr float px(radius value) noexcept {
  return static_cast<float>(static_cast<int>(value));
}

[[nodiscard]] constexpr float concentric(float outer, float inset) noexcept {
  return std::max(0.0F, outer - inset);
}

[[nodiscard]] constexpr float concentric(radius outer, space inset) noexcept {
  return concentric(px(outer), px(inset));
}

// Small type scale: point size, weight (a 0..1 hint for the renderer's font
// selector) and line height as a multiple of the size. Only these five styles
// are allowed. Value and label share a size so swapped values do not change
// the rhythm of a list.
struct type_style {
  float size;
  float weight;
  float line_height;
};

inline constexpr type_style title{34.0F, 0.72F, 1.20F};
inline constexpr type_style label{30.0F, 0.65F, 1.20F};
inline constexpr type_style value{30.0F, 0.55F, 1.20F};
inline constexpr type_style caption{18.0F, 0.65F, 1.22F};
inline constexpr type_style hint{24.0F, 0.55F, 1.20F};

// The panel has rounded corners and a safe area; nothing is drawn against its
// edge. Status bar, FPS counter and hint bar all inset by this much.
inline constexpr float safe_inset = px(space::lg);

} // namespace openxmb::xmb::ui
