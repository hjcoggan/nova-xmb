// Contract tests for the console's one spacing, radius and type scale.
#include "openxmb/xmb/ui_tokens.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace ui = openxmb::xmb::ui;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "ui tokens contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool near(float a, float b, float epsilon = 0.001F) {
  return std::abs(a - b) <= epsilon;
}

void spacing_scale_is_a_real_scale() {
  require(ui::px(ui::space::xxs) == 4.0F, "xxs is 4");
  require(ui::px(ui::space::xs) == 8.0F, "xs is 8");
  require(ui::px(ui::space::sm) == 12.0F, "sm is 12");
  require(ui::px(ui::space::md) == 16.0F, "md is 16");
  require(ui::px(ui::space::lg) == 24.0F, "lg is 24");
  require(ui::px(ui::space::xl) == 32.0F, "xl is 32");
  require(ui::px(ui::space::xxs) < ui::px(ui::space::xs) &&
              ui::px(ui::space::xs) < ui::px(ui::space::sm) &&
              ui::px(ui::space::sm) < ui::px(ui::space::md) &&
              ui::px(ui::space::md) < ui::px(ui::space::lg) &&
              ui::px(ui::space::lg) < ui::px(ui::space::xl),
          "spacing increases monotonically");
}

void radii_are_concentric_with_spacing() {
  require(ui::px(ui::radius::xs) == 8.0F, "xs radius is 8");
  require(ui::px(ui::radius::sm) == 12.0F, "sm radius is 12");
  require(ui::px(ui::radius::md) == 20.0F, "md radius is 20");
  require(ui::px(ui::radius::lg) == 28.0F, "lg radius is 28");
  require(ui::px(ui::radius::xl) == 36.0F, "xl radius is 36");
  require(near(ui::concentric(ui::radius::xl, ui::space::xs),
               ui::px(ui::radius::lg)),
          "36 minus 8 is 28");
  require(near(ui::concentric(ui::radius::lg, ui::space::xs),
               ui::px(ui::radius::md)),
          "28 minus 8 is 20");
  require(near(ui::concentric(ui::radius::md, ui::space::xs),
               ui::px(ui::radius::sm)),
          "20 minus 8 is 12");
  require(near(ui::concentric(ui::radius::sm, ui::space::xxs),
               ui::px(ui::radius::xs)),
          "12 minus 4 is 8");
  require(ui::concentric(4.0F, 8.0F) == 0.0F, "radius never goes negative");
}

void type_scale_covers_the_hierarchy() {
  require(ui::title.size > ui::label.size, "title is largest");
  require(near(ui::label.size, ui::value.size), "label and value share a size");
  require(ui::caption.size < ui::hint.size && ui::hint.size < ui::label.size,
          "caption < hint < label");
  require(ui::title.line_height > 1.0F && ui::caption.line_height > 1.0F,
          "every style has a line height");
  require(ui::safe_inset >= ui::px(ui::space::md),
          "content clears the panel edge");
}

} // namespace

int main() {
  spacing_scale_is_a_real_scale();
  radii_are_concentric_with_spacing();
  type_scale_covers_the_hierarchy();
  std::cout << "ui tokens: all contracts hold\n";
  return EXIT_SUCCESS;
}
