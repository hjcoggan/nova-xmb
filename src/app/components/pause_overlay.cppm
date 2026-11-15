/* Nova pause overlay for OpenXMB (ROADMAP B3-B5). */

module;

#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include "openxmb/xmb/glass.hpp"

export module openxmb.app:pause_overlay;

import auroreengine;
import openxmb.utils;
import :component;

namespace app {

export class pause_overlay : public component, public action_receiver {
    public:
        explicit pause_overlay(std::string game_id);

        [[nodiscard]] bool is_opaque() const override { return true; }
        [[nodiscard]] bool do_fade_in() const override { return true; }
        [[nodiscard]] bool do_fade_out() const override { return true; }

        void render(auroreengine::gui_renderer& renderer, class shell* xmb) override;
        result on_action(action action) override;

        [[nodiscard]] const std::string& game_id() const noexcept { return current_game_id; }

        // The game is being stopped: the panel says so, takes no input and
        // stays until the shell removes it when the game's process is gone.
        void begin_closing();
        [[nodiscard]] bool closing() const noexcept { return closing_since.has_value(); }
        [[nodiscard]] std::chrono::steady_clock::duration closing_for() const {
            return closing_since ? std::chrono::steady_clock::now() - *closing_since
                                 : std::chrono::steady_clock::duration::zero();
        }

    private:
        std::string current_game_id;
        std::string current_game_title;
        int selected = 0;
        int volume_pct = 50;
        int brightness_pct = 50;
        std::vector<std::string> guide_lines;
        std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> closing_since;
        openxmb::xmb::glass::focus_tracker focus;

        void refresh_levels();
};

} // namespace app
