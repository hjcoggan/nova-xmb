#pragma once

// The still of a paused game that the pause panel's glass sits on. The menu
// has no copy of the game's pixels, so it captures one when PAUSE arrives;
// the panel waits for it, but never longer than capture_budget_ms.

namespace openxmb::xmb {

enum class pause_backdrop_state { idle, capturing, ready, failed };

struct pause_backdrop {
  static constexpr float capture_budget_ms = 250.0F;

  pause_backdrop_state state = pause_backdrop_state::idle;

  void start() noexcept { state = pause_backdrop_state::capturing; }

  // A capture finished. Ignored unless one is in progress, so a still that
  // lands after RESUME or QUIT is never shown.
  void finish(bool ok) noexcept {
    if (state == pause_backdrop_state::capturing)
      state = ok ? pause_backdrop_state::ready : pause_backdrop_state::failed;
  }

  void cancel() noexcept { state = pause_backdrop_state::idle; }

  [[nodiscard]] bool panel_may_show(float elapsed_ms) const noexcept {
    return state == pause_backdrop_state::ready || state == pause_backdrop_state::failed ||
           elapsed_ms >= capture_budget_ms;
  }

  [[nodiscard]] bool draw_still() const noexcept { return state == pause_backdrop_state::ready; }
};

}  // namespace openxmb::xmb
