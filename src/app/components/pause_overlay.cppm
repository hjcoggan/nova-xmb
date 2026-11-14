/* Nova pause overlay for OpenXMB (ROADMAP B3-B5). */

module;

#include <chrono>
#include <string>

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

    private:
        std::string current_game_id;
        int selected = 0;
        int volume_pct = 50;
        int brightness_pct = 50;
        std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

        void refresh_levels();
};

} // namespace app
