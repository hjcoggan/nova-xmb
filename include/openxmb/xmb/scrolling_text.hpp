#pragma once

// One reusable text slot for every string the console can vary: game titles,
// setting labels and values, Wi-Fi / Bluetooth names, dialog titles and hint
// text. It holds a borrowed view of the string and stores only indices and
// timings, so sampling it costs no allocation and no GPU work.
//
// Unfocused: the text is truncated with an ellipsis glyph.
// Focused: hold, scroll to the end, pause at the end, then loop — the iPod
// treatment. Only the focused item animates.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace openxmb::xmb::ui {

namespace text_timing {
inline constexpr double hold_start_ms = 600.0;
inline constexpr double scroll_chars_per_sec = 5.0;
inline constexpr double hold_end_ms = 900.0;
} // namespace text_timing

// What a renderer needs for one frame. `first`..`first+count` are glyph
// indices into the original string; `offset_px` shifts them left by a
// fraction of a glyph for smooth motion. `ellipsis` asks for one extra glyph
// after the range. Drawing is clipped to the slot by the caller.
struct text_run {
  std::size_t first{};
  std::size_t count{};
  float offset_px{};
  bool ellipsis{};
};

enum class text_phase { fits, truncated, hold_start, scrolling, hold_end };

class scrolling_text {
 public:
  scrolling_text() noexcept = default;

  // Bind a new string. `fitting` is the number of glyphs the slot can show;
  // a caller computes it once with measure_text (see fitting_count below).
  void set_text(std::string_view text, std::size_t fitting, bool focused,
                double now_ms) noexcept {
    text_ = text;
    fitting_ = fitting;
    focused_ = focused;
    started_ms_ = now_ms;
  }

  void set_focused(bool focused, double now_ms) noexcept {
    if (focused_ == focused) return;
    focused_ = focused;
    started_ms_ = now_ms;
  }

  [[nodiscard]] std::size_t fitting() const noexcept { return fitting_; }
  [[nodiscard]] bool fits() const noexcept {
    return fitting_ == 0 || text_.size() <= fitting_;
  }
  [[nodiscard]] bool focused() const noexcept { return focused_; }

  [[nodiscard]] text_phase phase(double now_ms) const noexcept {
    if (fits()) return text_phase::fits;
    if (!focused_) return text_phase::truncated;
    return motion(now_ms).phase;
  }

  [[nodiscard]] text_run sample(double now_ms, float glyph_width_px) const
      noexcept {
    const auto fitting = std::size_t{fitting_};
    if (fits()) return {0, text_.size(), 0.0F, false};
    if (!focused_) {
      // Leave one glyph for the ellipsis; a single-character slot shows just
      // the ellipsis rather than clipping a half glyph.
      const auto room = fitting > 0 ? fitting - 1 : 0;
      return {0, room, 0.0F, true};
    }

    const auto [phase, cursor] = motion(now_ms);
    (void)phase;
    const auto first = static_cast<std::size_t>(cursor);
    const auto whole = static_cast<float>(first);
    // One extra glyph covers the fractional shift; the caller clips it.
    const auto count = std::min(fitting + 1, text_.size() - first);
    return {first, count,
            static_cast<float>((cursor - whole) * glyph_width_px), false};
  }

 private:
  struct motion_state {
    text_phase phase;
    double cursor;
  };

  [[nodiscard]] motion_state motion(double now_ms) const noexcept {
    const auto end = static_cast<double>(text_.size() - fitting_);
    const auto start = text_timing::hold_start_ms;
    const auto travel = end / text_timing::scroll_chars_per_sec * 1000.0;
    const auto stop = start + travel;
    const auto total = stop + text_timing::hold_end_ms;
    const auto elapsed = now_ms - started_ms_;
    // A clock that moved backwards (or a stale sample) shows the first page.
    if (elapsed <= 0.0 || total <= 0.0) return {text_phase::hold_start, 0.0};
    const auto t = elapsed >= total ? std::fmod(elapsed, total) : elapsed;
    if (t < start) return {text_phase::hold_start, 0.0};
    if (t < stop) {
      return {text_phase::scrolling,
              (t - start) / 1000.0 * text_timing::scroll_chars_per_sec};
    }
    return {text_phase::hold_end, end};
  }

  std::string_view text_{};
  std::size_t fitting_{};
  bool focused_{};
  double started_ms_{};
};

// Measure how many glyphs fit in `max_width`. The measure callback takes a
// borrowed prefix and returns its width in the same units; it is called only
// when a slot's text changes, never per frame from this component.
template <class Measure>
[[nodiscard]] std::size_t fitting_count(std::string_view text, float max_width,
                                        Measure&& measure,
                                        std::size_t ceiling = 512) noexcept {
  if (text.empty() || max_width <= 0.0F) return 0;
  const auto limit = std::min(text.size(), ceiling);
  std::size_t count = 1;
  while (count < limit &&
         measure(text.substr(0, count + 1)) <= max_width) {
    ++count;
  }
  return count;
}

} // namespace openxmb::xmb::ui
