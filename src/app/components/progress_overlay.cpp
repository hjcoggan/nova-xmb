/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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
#include "openxmb/xmb/glass.hpp"

#include "dialog_text_layout.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module openxmb.app;
import :progress_overlay;

import :message_overlay;
import :xmb_presentation;
import openxmb.config;

import auroreengine;
import glm;
import spdlog;

namespace app {
    progress_overlay::progress_overlay(std::string title, std::unique_ptr<progress_item>&& item, bool show_progress,
            std::function<void(class shell*, bool)> on_complete)
        : title(std::move(title)), item(std::move(item)), show_progress(show_progress), on_complete(std::move(on_complete))
    {
        auto status = this->item->init(status_message);
        if(status == progress_item::status::error) {
            failed = true;
        } else if(status == progress_item::status::success) {
            done = true;
        }
    }

    result progress_overlay::tick(shell* xmb) {
        if(!failed && !done) {
            auto status = item->progress(progress, status_message);
            if(status == progress_item::status::error) {
                failed = true;
            } else if(status == progress_item::status::success) {
                done = true;
            }
        }

        if(failed) {
            spdlog::error("Progress failed: \"{}\"", status_message);
            if(on_complete) {
                on_complete(xmb, false);
            } else if(!status_message.empty()) {
                xmb->emplace_overlay<message_overlay>(title, status_message, std::vector<std::string>{"OK"});
            }
            return result::failure | result::close;
        }
        if(done) {
            spdlog::info("Progress done: \"{}\"", status_message);
            if(on_complete) {
                on_complete(xmb, true);
            } else if(!status_message.empty()) {
                xmb->emplace_overlay<message_overlay>(title, status_message, std::vector<std::string>{"OK"});
            }
            return result::success | result::close;
        }
        return result::success;
    }

    void progress_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
        overlay_canvas c(renderer);
        const float vis_left = c.visible_left();
        const float vis_right = c.visible_right();
        const float vis_top = c.visible_top();
        const float vis_bottom = c.visible_bottom();
        const float span_w = vis_right - vis_left;
        const float span_h = vis_bottom - vis_top;
        const float vis_cx = (vis_left + vis_right) * 0.5f;
        const float vis_cy = (vis_top + vis_bottom) * 0.5f;

        namespace glass = openxmb::xmb::glass;
        const glm::vec4 primary{1.0f, 1.0f, 1.0f, glass::text_primary};
        c.rect(vis_left - 2.0f, vis_top - 2.0f, span_w + 4.0f, span_h + 4.0f, {0.0f, 0.0f, 0.0f, 0.30f});

        constexpr float margin = 40.0f;
        // Room for the hint bar below the card.
        constexpr float hint_reserve = 112.0f;
        const float max_card_w = std::max(280.0f, span_w - 2.0f * margin);
        const float max_card_h = std::max(220.0f, span_h - margin - hint_reserve);
        const float card_w = std::min(820.0f, max_card_w);
        const float content_width = std::max(160.0f, card_w - 120.0f);

        const float ui = config::CONFIG.textScale;
        const float body_pixels = 24.0f * c.scale * ui;
        const float title_pixels = 28.0f * c.scale * ui;

        auto title_lines = dialog_text::wrap(title, content_width * c.scale,
            [&](auto v) { return renderer.text_width_pixels(v, title_pixels); });
        if(title_lines.size() > 2) {
            title_lines.resize(2);
            title_lines.back() = dialog_text::ellipsize(title_lines.back() + "...", content_width * c.scale,
                [&](auto v) { return renderer.text_width_pixels(v, title_pixels); });
        }

        std::vector<std::string> status_lines;
        if(!status_message.empty()) {
            status_lines = dialog_text::wrap(status_message, content_width * c.scale,
                [&](auto v) { return renderer.text_width_pixels(v, body_pixels); });
        }

        const auto title_metrics = renderer.text_line_metrics_pixels(title_pixels) / c.scale;
        const auto body_metrics = renderer.text_line_metrics_pixels(body_pixels) / c.scale;

        const float title_line_h = title_metrics.x + title_metrics.y;
        const float title_pitch = title_line_h + 8.0f * ui;
        const float title_block_h = title_lines.empty()
            ? 0.0f
            : title_line_h + static_cast<float>(title_lines.size() - 1) * title_pitch;

        const float body_line_h = body_metrics.x + body_metrics.y;
        const float body_pitch = body_line_h + 8.0f * ui;

        constexpr float top_pad = glass::card_padding;
        const float title_status_gap = (!title_lines.empty() && !status_lines.empty()) ? 16.0f : 0.0f;
        constexpr float track_h = 12.0f;
        const float bar_gap = show_progress ? 24.0f : 0.0f;
        const float bar_pct_gap = show_progress ? 16.0f : 0.0f;
        const float pct_h = show_progress ? body_line_h : 0.0f;
        constexpr float bottom_gap = 0.0f;
        constexpr float footer_h = glass::card_padding;

        const float fixed_h = top_pad + title_block_h + title_status_gap +
            bar_gap + (show_progress ? track_h : 0.0f) + bar_pct_gap + pct_h +
            bottom_gap + footer_h;

        std::size_t visible_status = 0;
        float status_block_h = 0.0f;
        if(!status_lines.empty()) {
            const float max_status_h = std::max(body_line_h, max_card_h - fixed_h);
            const std::size_t cap = dialog_text::line_capacity(max_status_h, body_pitch, body_line_h);
            visible_status = std::min(status_lines.size(), cap);
            status_block_h = body_line_h + static_cast<float>(visible_status - 1) * body_pitch;
        }

        const float card_h = std::min(max_card_h, fixed_h + status_block_h);
        const float card_x = vis_cx - card_w * 0.5f;
        const float card_y = std::clamp(vis_cy - card_h * 0.5f - hint_reserve * 0.5f, vis_top + margin,
            std::max(vis_top + margin, vis_bottom - hint_reserve - card_h));

        c.glass(card_x, card_y, card_w, card_h);

        const float title_top = card_y + top_pad;
        for(std::size_t i = 0; i < title_lines.size(); ++i) {
            c.text(title_lines[i], vis_cx, title_top + title_metrics.x + static_cast<float>(i) * title_pitch,
                28, primary, 0.0f, true);
        }

        const float status_top = title_top + title_block_h + title_status_gap;
        for(std::size_t i = 0; i < visible_status; ++i) {
            c.text(status_lines[i], vis_cx, status_top + body_metrics.x + static_cast<float>(i) * body_pitch,
                24, primary, 0.0f, true);
        }

        if(show_progress) {
            const float track_w = std::min(560.0f, content_width);
            const float track_x = vis_cx - track_w * 0.5f;
            const float track_y = status_top + status_block_h + bar_gap;
            const float pct = static_cast<float>(std::clamp(progress, 0.0, 1.0));
            const float fill_w = track_w * pct;

            const auto accent_rgb = glass::active_accent_linear();
            const glm::vec4 accent{accent_rgb[0], accent_rgb[1], accent_rgb[2], 1.0f};
            c.rect(track_x, track_y, track_w, track_h, {1.0f, 1.0f, 1.0f, 0.25f}, track_h * 0.5f);
            if(fill_w > 1.0f) {
                c.rect(track_x, track_y, fill_w, track_h, accent, std::min(track_h, fill_w) * 0.5f);
            }

            const float pct_baseline = track_y + track_h + bar_pct_gap + body_metrics.x;
            c.text(std::format("{:.0f}%", pct * 100.0f), vis_cx, pct_baseline,
                24, {1.0f, 1.0f, 1.0f, glass::text_secondary}, 0.0f, true);
        }

        const auto previous_scale = renderer.text_scale;
        renderer.text_scale = 1.0f;
        xmb->render_hint_bar(renderer, std::array{
            std::pair{action::none, ""},
            std::pair{action::cancel, "Back"}
        });
        renderer.text_scale = previous_scale;
    }

    result progress_overlay::on_action(action action) {
        switch(action) {
            case action::cancel:
                return item->cancel(status_message) ? result::success | result::close : result::unsupported | result::error_rumble;
            default:
                return result::unsupported;
        }
    }
}
