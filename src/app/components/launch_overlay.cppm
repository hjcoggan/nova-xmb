/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

module;

#include <chrono>
#include <future>
#include <memory>
#include <string>

export module openxmb.app:launch_overlay;

import auroreengine;
import sdl2;
import vulkan_hpp;
import vma;
import openxmb.utils;
import :component;

namespace app {

// The loading screen shown while a game starts and has no window yet. A cold
// Android or Proton start takes tens of seconds and loads the whole machine,
// so this covers the screen and is cheap to draw: the menu and its wallpaper
// are not rendered behind it. It takes no input; the shell closes it when the
// game's window appears or the launch ends.
export class launch_overlay : public component {
    public:
        launch_overlay(std::string title, const std::string& artwork, auroreengine::resource_loader& loader);
        ~launch_overlay() override;

        [[nodiscard]] bool is_opaque() const override { return true; }
        [[nodiscard]] std::array<float, 3> backdrop_srgb() const override { return {8.0F / 255.0F, 16.0F / 255.0F, 40.0F / 255.0F}; }
        [[nodiscard]] bool covers_scene() const override;
        [[nodiscard]] bool do_fade_in() const override { return false; }
        [[nodiscard]] bool do_fade_out() const override { return true; }
        [[nodiscard]] bool keeps_awake() const override { return true; }
        void render(auroreengine::gui_renderer& renderer, class shell* xmb) override;

        [[nodiscard]] std::chrono::steady_clock::duration age() const {
            return std::chrono::steady_clock::now() - start_time;
        }
    private:
        std::string title;
        std::unique_ptr<auroreengine::texture> artwork;
        std::future<void> artwork_ready;
        std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};
};

}
