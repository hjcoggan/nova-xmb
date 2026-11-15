module;

#include "openxmb/xmb/settings_scene.hpp"
#include "openxmb/xmb/background.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

export module openxmb.xmb.settings_scene_renderer;

import auroreengine;
import openxmb.config;
import openxmb.utils;
import glm;

export namespace openxmb::xmb {

struct SettingsSceneIcon {
  std::string_view semantic_id;
  const auroreengine::texture *texture{};
  const auroreengine::texture *glass_texture{};
  bool preserve_color{};
};

struct SettingsSceneRenderOptions {
  bool glass_icons{};
  bool suppress_value_labels{};
};

class SettingsSceneRenderer {
public:
  void render(auroreengine::gui_renderer &renderer,
              const SettingsSceneSnapshot &snapshot,
              std::span<const SettingsSceneIcon> icons,
              SettingsSceneRenderOptions options = {}) const;
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

inline constexpr double settings_focused_glass_alpha = 0.90;
inline constexpr double settings_inactive_glass_alpha = 0.90;

struct ContainedLayout {
  float scale{};
  float offset_x{};
  float offset_y{};
  float framebuffer_width{};
  float framebuffer_height{};

  [[nodiscard]] float x(double logical_x) const noexcept {
    return static_cast<float>((offset_x + logical_x * scale) /
                              framebuffer_width);
  }
  [[nodiscard]] float y(double logical_y) const noexcept {
    return static_cast<float>((offset_y + logical_y * scale) /
                              framebuffer_height);
  }
  [[nodiscard]] float image_extent(double logical_extent) const noexcept {
    // Aurore images use framebuffer-height units on both axes and perform
    // aspect correction internally.
    return static_cast<float>(logical_extent * scale / framebuffer_height);
  }
  [[nodiscard]] float text_size(double logical_size) const noexcept {
    return image_extent(logical_size * 2.5);
  }
};

[[nodiscard]] ContainedLayout
make_layout(const auroreengine::gui_renderer &renderer) noexcept {
  const auto width = static_cast<float>(renderer.frame_size.width);
  const auto height = static_cast<float>(renderer.frame_size.height);
  const auto base_scale = static_cast<double>(height) / SettingsSceneMetrics::logical_height;
  const auto ui = config::CONFIG.ui_scale();
  const auto scale = static_cast<float>(base_scale * ui);
  const auto base_ox = (static_cast<double>(width) - SettingsSceneMetrics::logical_width * base_scale) * 0.5;
  const auto anchor_x = (static_cast<double>(width) / std::max(1.0, static_cast<double>(height)) <
                         SettingsSceneMetrics::logical_width / SettingsSceneMetrics::logical_height)
      ? (static_cast<double>(width) * 0.28)
      : (base_ox + 566.0 * base_scale);
  const auto target_y = (ui > 1.0) ? (static_cast<double>(height) * 0.27) : (289.0 * base_scale);
  return {
      .scale = scale,
      .offset_x = static_cast<float>(anchor_x - 566.0 * scale),
      .offset_y = static_cast<float>(target_y - (ui > 1.0 ? 279.0 : 289.0) * scale),
      .framebuffer_width = width,
      .framebuffer_height = height,
  };
}

[[nodiscard]] const SettingsSceneIcon *
find_icon(std::span<const SettingsSceneIcon> icons,
          std::string_view semantic_id) noexcept {
  for (const auto &icon : icons) {
    if (icon.semantic_id == semantic_id)
      return &icon;
  }
  return nullptr;
}

void draw_icon(auroreengine::gui_renderer &renderer,
               const ContainedLayout &layout, const SettingsRowVisual &row,
               const SettingsSceneIcon &icon, bool glass) {
  const bool use_glass =
      !icon.preserve_color && glass &&
      icon.glass_texture != nullptr && icon.glass_texture->loaded;
  const auroreengine::texture *target =
      use_glass ? icon.glass_texture : icon.texture;
  if (target == nullptr || !target->loaded) {
    return;
  }

  // Covers use the same fixed card as the crossbar (SelectionLens).
  // On the console every icon fits the shared row box; elsewhere only covers.
  const bool boxed = icon.preserve_color || config::CONFIG.console_mode();
  const double slot_w = boxed ? SelectionLens::box_width(row.icon_extent) : row.icon_extent;
  const double slot_h = boxed ? SelectionLens::box_height(row.icon_extent) : row.icon_extent;
  double draw_w = slot_w;
  double draw_h = slot_h;
  if (target->width > 0 && target->height > 0) {
    const double tex_w = static_cast<double>(target->width);
    const double tex_h = static_cast<double>(target->height);
    const double fit = std::min(slot_w / tex_w, slot_h / tex_h);
    draw_w = tex_w * fit;
    draw_h = tex_h * fit;
  }

  const auto x = layout.x(row.icon_center_x - draw_w * 0.5);
  const auto y = layout.y(row.center_y - draw_h * 0.5);
  const auto w = layout.image_extent(draw_w);
  const auto h = layout.image_extent(draw_h);
  if (icon.preserve_color) {
    renderer.draw_image(*target, x, y, w, h,
                        glm::vec4(1.0F, 1.0F, 1.0F, static_cast<float>(row.alpha)));
    return;
  }
  const auto glass_alpha = row.focused
                               ? std::min(settings_focused_glass_alpha,
                                          row.alpha * settings_focused_glass_alpha)
                               : row.alpha * settings_inactive_glass_alpha;
  const auto tint =
      glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), static_cast<float>(use_glass ? glass_alpha : row.alpha));
  if (use_glass)
    renderer.draw_image_glass(*target, x, y, w, h, tint);
  else
    renderer.draw_image(*target, x, y, w, h, tint);
}

// The same clear-glass lens the crossbar puts under its selected row
// (app::xmb_canvas::glass_clear), drawn in this renderer's layout space.
void draw_focus_lens(auroreengine::gui_renderer &renderer,
                     const ContainedLayout &layout, const SettingsRowVisual &row,
                     double right_x) {
  const auto alpha = static_cast<float>(row.alpha);
  if (alpha <= 0.001F) return;
  const auto rounded = [&](double x, double y, double w, double h,
                           glm::vec4 colour, double radius) {
    if (w <= 0 || h <= 0) return;
    auroreengine::simple_params parameters{};
    parameters.aspect_ratio = static_cast<float>(w / h);
    parameters.border_radius.fill(static_cast<float>(std::min(radius / h, 0.5)));
    renderer.draw_rect({layout.x(x), layout.y(y)},
                       {static_cast<float>(w * layout.scale / layout.framebuffer_width),
                        static_cast<float>(h * layout.scale / layout.framebuffer_height)},
                       colour, parameters);
  };
  const auto linear = [](float v) {
    v /= 255.0F;
    return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
  };
  constexpr double rim = 1.6, radius = SelectionLens::radius;
  const double left = SelectionLens::left(row.icon_center_x, row.icon_extent);
  const double height = SelectionLens::height(row.icon_extent);
  const double top = row.center_y - height * 0.5;
  const double width = right_x - left;
  rounded(left, top, width, height,
          {linear(205), linear(222), linear(255), 0.34F * alpha}, radius);
  rounded(left + rim, top + rim, width - 2 * rim, height - 2 * rim,
          {linear(20), linear(44), linear(110), 0.34F * alpha}, radius - rim);
  // Sheen on the upper half.
  rounded(left + rim, top + rim, width - 2 * rim, (height - 2 * rim) * 0.5,
          {1, 1, 1, 0.05F * alpha}, radius - rim);
}

void draw_focused_label(auroreengine::gui_renderer &renderer,
                        const ContainedLayout &layout,
                        const SettingsRowVisual &row) {
  const float pulse=0.8F+0.4F*std::sin(static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count())*1.848F);
  renderer.draw_text_pixels(row.label,layout.offset_x+row.label_x*layout.scale,
      layout.offset_y+(row.center_y+3.0)*layout.scale,row.label_size*layout.scale,
      {1,1,1,static_cast<float>(row.alpha)},0.72F,pulse);

}

void draw_wrapped_description(auroreengine::gui_renderer &renderer,
                              const ContainedLayout &layout,
                              const SettingsRowVisual &row) {
  if (!row.has_description || row.description_alpha <= 0.001 ||
      row.description_max_width <= 0.0)
    return;

  const auto size = layout.text_size(row.description_size);
  const auto avail_w = std::max(
      120.0,
      static_cast<double>(layout.framebuffer_width) -
          (layout.offset_x + row.description_x * layout.scale) -
          48.0 * layout.scale);
  const auto max_width = static_cast<float>(
      std::min(row.description_max_width * layout.scale, avail_w) /
      layout.framebuffer_width);
  auto cursor = std::size_t{};
  auto line = std::size_t{};
  while (cursor < row.description.size() &&
         line < SettingsSceneMetrics::max_description_lines) {
    while (cursor < row.description.size() &&
           (row.description[cursor] == ' ' || row.description[cursor] == '\n'))
      ++cursor;
    if (cursor >= row.description.size())
      break;

    const auto line_start = cursor;
    auto accepted_end = cursor;
    while (cursor < row.description.size()) {
      const auto word_start = cursor;
      while (cursor < row.description.size() &&
             row.description[cursor] != ' ' && row.description[cursor] != '\n')
        ++cursor;
      const auto candidate_end = cursor;
      const auto candidate =
          row.description.substr(line_start, candidate_end - line_start);
      // Aurore's measurement consumes the pre-geometry half scale.
      if (accepted_end != line_start &&
          renderer.measure_text(candidate, size * 0.5F).x > max_width) {
        cursor = word_start;
        break;
      }
      accepted_end = candidate_end;
      if (cursor < row.description.size() && row.description[cursor] == '\n') {
        ++cursor;
        break;
      }
      while (cursor < row.description.size() && row.description[cursor] == ' ')
        ++cursor;
    }
    if (accepted_end == line_start)
      accepted_end = cursor;
    const auto text =
        row.description.substr(line_start, accepted_end - line_start);
    renderer.draw_text_pixels(text,layout.offset_x+row.description_x*layout.scale,
        layout.offset_y+(row.description_y+static_cast<double>(line)*row.description_line_height)*layout.scale,
        22.0F*layout.scale,{0.92F,0.92F,0.92F,static_cast<float>(row.description_alpha)},0.65F);
    ++line;
  }
}

} // namespace

void SettingsSceneRenderer::render(auroreengine::gui_renderer &renderer,
                                   const SettingsSceneSnapshot &snapshot,
                                   std::span<const SettingsSceneIcon> icons,
                                   SettingsSceneRenderOptions options) const {
  const auto layout = make_layout(renderer);
  const auto ui = config::CONFIG.ui_scale();
  const auto focus_y_shift = (ui > 1.0)
      ? (((layout.framebuffer_height * 0.50 - layout.offset_y) / layout.scale) -
         SettingsSceneMetrics::focus_y)
      : 0.0;
  for (std::size_t index = 0; index < snapshot.row_count; ++index) {
    auto row = snapshot.rows[index];
    if (row.alpha <= 0.001)
      continue;
    row.center_y += focus_y_shift;
    // The description belongs to its row: without this it stayed at the
    // unshifted position and ran into the row below.
    row.description_y += focus_y_shift;
    const auto screen_top_y =
        layout.y(row.center_y - row.icon_extent * 0.5) * layout.framebuffer_height;
    const auto screen_bottom_y =
        layout.y(row.center_y + row.icon_extent * 0.5) * layout.framebuffer_height;
    if (screen_top_y < 16.0)
      continue;
    const auto top_fade = (ui > 1.0)
        ? std::clamp((row.center_y - (289.0 + 80.0)) / 90.0, 0.0, 1.0)
        : 1.0;
    const auto bottom_fade = (ui > 1.0)
        ? std::clamp((layout.framebuffer_height * 0.89 - screen_bottom_y) /
                         (layout.framebuffer_height * 0.06),
                     0.0, 1.0)
        : 1.0;
    const auto edge_fade = std::min(top_fade, bottom_fade);
    if (edge_fade <= 0.001)
      continue;
    row.alpha *= edge_fade;
    if (config::CONFIG.console_mode()) {
      // Labels start where they do in the other columns.
      row.label_x += SelectionLens::label_shift;
      row.description_x += SelectionLens::label_shift;
    }
    row.value_alpha *= edge_fade;
    row.description_alpha *= edge_fade;

    if (row.focused && config::CONFIG.console_mode()) {
      // Span the label, or the value when the row has one.
      const auto label_px = renderer.text_width_pixels(
          row.label, static_cast<float>(row.label_size) * layout.scale) / layout.scale;
      double right = row.label_x + label_px + SelectionLens::right_padding;
      if (row.has_description && !row.description.empty()) {
        // The description is drawn at 22 px and wraps at its maximum width.
        const auto description_px = std::min(
            static_cast<double>(renderer.text_width_pixels(row.description, 22.0F * layout.scale)) / layout.scale,
            row.description_max_width);
        right = std::max(right, row.description_x + description_px + SelectionLens::right_padding);
      }
      if (!options.suppress_value_labels && row.has_value) {
        right = std::max(right, std::min(row.value_right_x,
            (layout.framebuffer_width - layout.offset_x) / layout.scale - 48.0) + 24.0);
      }
      draw_focus_lens(renderer, layout, row, right);
    }
    if (const auto *icon = find_icon(icons, row.icon_ref))
      draw_icon(renderer, layout, row, *icon, options.glass_icons);

    if (row.focused) {
      draw_focused_label(renderer, layout, row);
      draw_wrapped_description(renderer, layout, row);
    } else {
      const auto value = 235.0F / 255.0F;
      renderer.draw_text_pixels(row.label,layout.offset_x+row.label_x*layout.scale,
          layout.offset_y+(row.center_y+11.0)*layout.scale,row.label_size*layout.scale,
          {value,value,value,static_cast<float>(row.alpha)},0.7F);
    }

    if (!options.suppress_value_labels && row.has_value &&
        row.value_alpha > 0.001) {
      const auto size = static_cast<float>(row.value_size)*layout.scale;
      // The value lives between the end of its label and the right margin;
      // a long one is shortened rather than drawn over the label.
      const auto label_size = static_cast<float>(row.label_size)*layout.scale;
      const auto label_end = layout.offset_x + row.label_x*layout.scale +
          renderer.text_width_pixels(row.label,label_size) + 36.0F*layout.scale;
      const auto right_edge = std::min(
          static_cast<double>(layout.offset_x + row.value_right_x * layout.scale),
          static_cast<double>(layout.framebuffer_width) - 48.0 * layout.scale);
      const auto room = static_cast<float>(right_edge - label_end);
      std::string value(row.value);
      if (room <= 0.0F) {
        value.clear();
      } else if (renderer.text_width_pixels(value,size) > room) {
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!value.empty() && renderer.text_width_pixels(value+ellipsis,size) > room) {
          value.pop_back();
          while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80) value.pop_back();
        }
        while (!value.empty() && value.back() == ' ') value.pop_back();
        value = value.empty() ? std::string{} : value + ellipsis;
      }
      const auto measured = renderer.text_width_pixels(value,size);
      const auto color =
          row.focused
              ? glm::vec3(245.0F / 255.0F, 240.0F / 255.0F, 250.0F / 255.0F)
              : glm::vec3(235.0F / 255.0F);
      const auto right_x = std::min(
          static_cast<double>(layout.offset_x + row.value_right_x * layout.scale),
          static_cast<double>(layout.framebuffer_width) - 48.0 * layout.scale);
      renderer.draw_text_pixels(value,
          right_x - measured,
          layout.offset_y+(row.center_y+(row.focused?3.0:11.0))*layout.scale,size,
          glm::vec4(color, static_cast<float>(row.value_alpha)),0.7F);
    }
  }
}

} // namespace openxmb::xmb
