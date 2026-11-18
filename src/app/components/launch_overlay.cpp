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
#include "openxmb/xmb/background.hpp"
#include "openxmb/xmb/glass.hpp"

#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

module openxmb.app;
import :launch_overlay;

import auroreengine;
import :xmb_presentation;
import glm;
import openxmb.config;
import openxmb.utils;

namespace app {
    namespace {
        constexpr float fade_in_seconds = 0.30F;

        float smooth01(float value) {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }

        struct launch_theme_colors {
            std::array<float, 3> top_srgb{};
            std::array<float, 3> bottom_srgb{};
            std::array<float, 3> glow_srgb{};
        };

        launch_theme_colors resolve_launch_theme() {
            const auto wall_now = openxmb::xmb::wall_clock_now();
            const auto theme = utils::xmb_resolve_theme_colour(wall_now);
            const auto wall_time = std::chrono::system_clock::to_time_t(wall_now);
            std::tm local_time{};
#if defined(_WIN32)
            localtime_s(&local_time, &wall_time);
#else
            localtime_r(&wall_time, &local_time);
#endif
            const float local_hour = static_cast<float>(local_time.tm_hour) +
                static_cast<float>(local_time.tm_min) / 60.0F +
                static_cast<float>(local_time.tm_sec) / 3600.0F;
            const float day_night_hour = utils::xmb_effective_day_night_hour(local_hour);
            const auto gradient = config::CONFIG.themeOriginalColour
                ? openxmb::xmb::resolve_background_gradient(local_time.tm_mon, day_night_hour)
                : openxmb::xmb::resolve_manual_background_gradient(
                    {theme.base_colour.r, theme.base_colour.g, theme.base_colour.b}, day_night_hour);
            const auto scale = [](std::array<float, 3> rgb, float factor) {
                return std::array<float, 3>{
                    std::clamp(rgb[0] * factor, 0.0F, 1.0F),
                    std::clamp(rgb[1] * factor, 0.0F, 1.0F),
                    std::clamp(rgb[2] * factor, 0.0F, 1.0F),
                };
            };
            return {
                .top_srgb = scale(gradient.bottom_rgb, 0.65F),
                .bottom_srgb = scale(gradient.top_rgb, 0.20F),
                .glow_srgb = {
                    std::clamp(theme.base_colour.r, 0.0F, 1.0F),
                    std::clamp(theme.base_colour.g, 0.0F, 1.0F),
                    std::clamp(theme.base_colour.b, 0.0F, 1.0F),
                },
            };
        }
    }

    launch_overlay::launch_overlay(std::string title, const std::string& artwork_path,
        auroreengine::resource_loader& loader) : title(std::move(title))
    {
        std::error_code error;
        if(!artwork_path.empty() && std::filesystem::is_regular_file(artwork_path, error)) {
            artwork = std::make_unique<auroreengine::texture>(loader.getDevice(), loader.getAllocator());
            artwork_ready = loader.loadTexture(artwork.get(), artwork_path);
        }
    }

    launch_overlay::~launch_overlay() {
        if(artwork_ready.valid()) artwork_ready.wait();
    }

    std::array<float, 3> launch_overlay::backdrop_srgb() const {
        return resolve_launch_theme().top_srgb;
    }

    bool launch_overlay::covers_scene() const {
        return std::chrono::duration<float>(age()).count() > fade_in_seconds;
    }

    void launch_overlay::render(auroreengine::gui_renderer& renderer, shell*) {
        const float seconds = std::chrono::duration<float>(age()).count();
        const float reveal = smooth01(seconds / fade_in_seconds);

        overlay_canvas c(renderer);
        const float left = c.visible_left(), right = c.visible_right();
        const float top = c.visible_top(), bottom = c.visible_bottom();
        const float width = right - left, height = bottom - top;
        const float cx = (left + right) * 0.5F;

        // A still backdrop matching the active theme, lit from above.
        const auto theme = resolve_launch_theme();
        c.gradient(left - 2, top - 2, width + 4, height + 4,
            xmb_canvas::srgb(theme.top_srgb[0] * 255.0F, theme.top_srgb[1] * 255.0F, theme.top_srgb[2] * 255.0F, reveal),
            xmb_canvas::srgb(theme.bottom_srgb[0] * 255.0F, theme.bottom_srgb[1] * 255.0F, theme.bottom_srgb[2] * 255.0F, reveal));
        c.soft_rect(cx - width * 0.6F, top - height * 0.34F, width * 1.2F, height * 0.80F,
            xmb_canvas::srgb(theme.glow_srgb[0] * 255.0F, theme.glow_srgb[1] * 255.0F, theme.glow_srgb[2] * 255.0F, 0.22F * reveal),
            height * 0.40F);

        renderer.push_color(glm::vec4(1.0F, 1.0F, 1.0F, reveal));

        // The game's banner on a glass card, at its own proportions.
        constexpr float card_max_w = 616.0F, card_max_h = 328.0F, pad = 24.0F;
        const bool has_artwork = artwork && artwork->loaded && artwork->width > 0 && artwork->height > 0;
        float card_bottom = top + height * 0.42F;
        if(has_artwork) {
            const float fit = std::min(card_max_w / static_cast<float>(artwork->width),
                                       card_max_h / static_cast<float>(artwork->height));
            const float w = static_cast<float>(artwork->width) * fit;
            const float h = static_cast<float>(artwork->height) * fit;
            const float y = top + height * 0.40F - h * 0.5F;
            c.glass(cx - w * 0.5F - pad, y - pad, w + pad * 2, h + pad * 2);
            c.image(*artwork, cx - w * 0.5F, y, w, h);
            card_bottom = y + h + pad;
        } else {
            c.text(title, cx, card_bottom + 92.0F, 46, {1, 1, 1, 1}, 0.35F, true);
        }

        // Three lights taking turns: readable at a low frame rate, and honest
        // about a wait that has no measurable progress.
        const float dots_y = has_artwork ? (card_bottom + 72.0F) : (card_bottom + 150.0F);
        for(int i = 0; i < 3; ++i) {
            const float phase = seconds * 1.6F - static_cast<float>(i) * 0.33F;
            const float lit = 0.5F + 0.5F * std::sin(phase * 2.0F * 3.14159265F / 1.6F);
            const float size = 13.0F + 5.0F * lit;
            c.rect(cx + (static_cast<float>(i) - 1.0F) * 38.0F - size * 0.5F, dots_y - size * 0.5F,
                size, size, {1.0F, 1.0F, 1.0F, 0.28F + 0.72F * lit}, size * 0.5F);
        }

        if(seconds > 9.0F) {
            const float hint = smooth01((seconds - 9.0F) / 0.8F);
            const auto text = openxmb::localization::shell_locale::translate(
                std::string("The first start can take a minute."));
            c.text(text, cx, dots_y + 74.0F, 24, {1, 1, 1, 0.62F * hint}, 0, true);
        }
        renderer.pop_color();
    }
}
