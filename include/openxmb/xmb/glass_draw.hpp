#pragma once

// Draws one pane of glass (openxmb/xmb/glass.hpp) through an AuroreEngine
// gui_renderer, in framebuffer pixels. A template so that it can live in a
// plain header included before the engine module is imported: instantiate it
// as draw_glass_px<auroreengine::glass_panel_params, auroreengine::simple_params>.

#include <algorithm>
#include <type_traits>

#include "openxmb/xmb/glass.hpp"

namespace openxmb::xmb::glass {

// x, y, w, h and radius in framebuffer pixels; unit is pixels per design unit
// (for the shadow's reach).
template <class PanelParams, class SimpleParams, class Renderer>
void draw_glass_px(Renderer& renderer, float x, float y, float w, float h, variant v, float radius, float alpha,
                   float unit) {
  if (w <= 0 || h <= 0 || alpha <= 0.001F) return;
  const auto fw = static_cast<float>(renderer.frame_size.width);
  const auto fh = static_cast<float>(renderer.frame_size.height);
  if (v != variant::focus) {
    // A faint shadow under every pane, offset slightly down.
    const float sx = x - 24 * unit, sy = y - 4 * unit, sw = w + 48 * unit, sh = h + 40 * unit;
    const float feather = std::min(48 * unit, sh * 0.5F);
    SimpleParams shadow{};
    using vec2 = std::remove_cvref_t<decltype(shadow.blur[0])>;
    const float fx = std::min(feather / sw, 0.5F), fy = std::min(feather / sh, 0.5F);
    shadow.blur = {vec2{0, fx}, vec2{0, fx}, vec2{0, fy}, vec2{0, fy}};
    renderer.draw_rect({sx / fw, sy / fh}, {sw / fw, sh / fh}, {0.0F, 0.0F, 0.0F, 0.18F * alpha}, shadow);
  }
  const auto m = material_for(v);
  PanelParams p{};
  p.size_px = {w, h};
  p.radius_px = clamp_radius(radius, w, h);
  p.alpha = alpha;
  p.refraction_px = m.refraction_px;
  p.blur_lod = m.blur_lod;
  p.tint = m.tint;
  p.dim_strength = m.dim_strength;
  p.samples_backdrop = m.samples_backdrop;
  if constexpr (requires { p.tint_rgba; }) {
    const auto tint_rgba = active_theme_tint_linear();
    p.tint_rgba = {tint_rgba[0], tint_rgba[1], tint_rgba[2], tint_rgba[3]};
  }
  const auto rim = draw_rim_for(v);
  renderer.draw_glass_panel({x / fw, y / fh}, {w / fw, h / fh}, {rim[0], rim[1], rim[2], rim[3]}, p);
}

}  // namespace openxmb::xmb::glass
