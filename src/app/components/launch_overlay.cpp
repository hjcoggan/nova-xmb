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

#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

module openxmb.app;
import :launch_overlay;

import auroreengine;
import :xmb_presentation;
import glm;
import openxmb.config;

namespace app {
    namespace {
        float smooth01(float value) {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }
    }

    launch_overlay::launch_overlay(std::string title) : title(std::move(title)) {}

    void launch_overlay::render(auroreengine::gui_renderer& renderer, shell*) {
        const float seconds = std::chrono::duration<float>(age()).count();
        renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, smooth01(seconds / 0.25f)));

        overlay_canvas c(renderer);
        // Below the crossbar, so the selected tile stays readable.
        c.rect(0, 0, 1920, 1080, {0, 0, 0, 0.35F});
        c.rect(0, 760, 1920, 190, {0, 0, 0, 0.55F});
        c.rect(0, 760, 1920, 1, {1, 1, 1, 0.55F});
        c.rect(0, 950, 1920, 1, {1, 1, 1, 0.55F});

        const auto heading = openxmb::localization::shell_locale::translate(std::string("Starting"));
        c.text(heading + " " + title, 960, 840, 36, {1, 1, 1, 1}, 0, true);

        // A bar sweeping back and forth: the launch has no measurable progress.
        constexpr float track_x = 710, track_w = 500, bar_w = 140, y = 885;
        c.rect(track_x, y, track_w, 6, {1, 1, 1, 0.18F}, 3);
        const float phase = 0.5F - 0.5F * std::cos(seconds * 2.4F);
        c.rect(track_x + (track_w - bar_w) * phase, y, bar_w, 6, {1, 1, 1, 0.85F}, 3);
        renderer.pop_color();
    }
}
