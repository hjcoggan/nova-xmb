// Contract tests for the one scrolling-text slot. No GPU, no renderer: the
// component stores only indices and timings, so these are pure logic tests.
#include "openxmb/xmb/scrolling_text.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace ui = openxmb::xmb::ui;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "scrolling text contract failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void short_text_never_scrolls() {
  ui::scrolling_text slot;
  slot.set_text("Clover Pit", 32, true, 0.0);
  require(slot.fits(), "a short title fits");
  auto run = slot.sample(10'000.0, 10.0F);
  require(run.first == 0 && run.count == 10 && !run.ellipsis &&
              run.offset_px == 0.0F,
          "focused short text is drawn whole and still");
  require(slot.phase(10'000.0) == ui::text_phase::fits, "phase is fits");
}

void unfocused_text_truncates_with_ellipsis() {
  constexpr std::string_view long_name =
      "The Extremely Long Name Of A Game That Will Not Fit In Its Box";
  ui::scrolling_text slot;
  slot.set_text(long_name, 12, false, 0.0);
  require(!slot.fits(), "a long title does not fit");
  const auto run = slot.sample(5'000.0, 10.0F);
  require(run.first == 0, "unfocused text starts at the beginning");
  require(run.count == 11, "one glyph is reserved for the ellipsis");
  require(run.ellipsis, "unfocused text ends with an ellipsis");
  require(run.offset_px == 0.0F, "unfocused text never moves");
  require(slot.phase(5'000.0) == ui::text_phase::truncated,
          "phase is truncated");
}

void focused_text_holds_scrolls_pauses_and_loops() {
  constexpr std::string_view long_name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  ui::scrolling_text slot;
  slot.set_text(long_name, 10, true, 0.0);
  const auto end = static_cast<double>(long_name.size() - 10); // 16 glyphs
  const auto travel = end / ui::text_timing::scroll_chars_per_sec * 1000.0;

  require(slot.phase(100.0) == ui::text_phase::hold_start,
          "it holds still when focused");
  require(slot.sample(100.0, 8.0F).first == 0, "hold shows the first page");

  constexpr float glyph_width = 8.0F;
  // 55% of the travel lands between glyph boundaries, so the run has a
  // fractional offset and slides smoothly.
  const double mid_ms = ui::text_timing::hold_start_ms + travel * 0.55;
  const auto mid = slot.sample(mid_ms, glyph_width);
  require(slot.phase(mid_ms) == ui::text_phase::scrolling,
          "it scrolls after the hold");
  require(mid.first > 0 && mid.first < static_cast<std::size_t>(end),
          "the window moves into the middle of the string");
  require(mid.count <= 11, "the renderer gets at most one spare glyph");
  require(mid.offset_px > 0.0F && mid.offset_px < glyph_width,
          "the run slides smoothly between glyph boundaries");

  const auto at_end = ui::text_timing::hold_start_ms + travel + 100.0;
  const auto last = slot.sample(at_end, 8.0F);
  require(slot.phase(at_end) == ui::text_phase::hold_end,
          "it pauses at the end");
  require(last.first == static_cast<std::size_t>(end),
          "the end pause shows the tail");

  const auto loop = ui::text_timing::hold_start_ms + travel +
                    ui::text_timing::hold_end_ms + 100.0;
  require(slot.sample(loop, 8.0F).first == 0, "it loops back to the start");
  require(slot.phase(loop) == ui::text_phase::hold_start,
          "the loop holds again");
}

void focus_changes_restart_the_hold() {
  constexpr std::string_view long_name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  ui::scrolling_text slot;
  slot.set_text(long_name, 10, false, 0.0);
  require(slot.phase(9'000.0) == ui::text_phase::truncated,
          "unfocused stays truncated however long it is looked at");
  slot.set_focused(true, 9'000.0);
  require(slot.phase(9'100.0) == ui::text_phase::hold_start,
          "focus restarts the hold from zero");
  require(slot.sample(9'100.0, 8.0F).first == 0,
          "focus always shows the beginning first");
  slot.set_focused(false, 9'200.0);
  require(slot.phase(99'000.0) == ui::text_phase::truncated,
          "losing focus stops the animation at once");
}

void new_text_resets_the_scroll() {
  ui::scrolling_text slot;
  slot.set_text("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 10, true, 0.0);
  const auto moved = slot.sample(2'000.0, 8.0F);
  require(moved.first > 0, "long text had scrolled");
  slot.set_text("A different long title that also will not fit", 10, true,
                2'000.0);
  require(slot.sample(2'100.0, 8.0F).first == 0,
          "a new string restarts the hold");
}

void fitting_count_uses_the_measure_callback() {
  constexpr std::string_view text = "abcdefghij";
  const auto n = ui::fitting_count(text, 25.0F,
                                   [](std::string_view prefix) {
                                     return static_cast<float>(prefix.size()) *
                                            10.0F;
                                   });
  require(n == 2, "only two 10px glyphs fit in 25px");
  require(ui::fitting_count("", 100.0F,
                            [](std::string_view) { return 0.0F; }) == 0,
          "an empty string needs no glyphs");
  require(ui::fitting_count("abc", 0.0F,
                            [](std::string_view) { return 0.0F; }) == 0,
          "a zero-width slot needs no glyphs");
}

} // namespace

int main() {
  short_text_never_scrolls();
  unfocused_text_truncates_with_ellipsis();
  focused_text_holds_scrolls_pauses_and_loops();
  focus_changes_restart_the_hold();
  new_text_resets_the_scroll();
  fitting_count_uses_the_measure_callback();
  std::cout << "scrolling text: all contracts hold\n";
  return EXIT_SUCCESS;
}
