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
#include "openxmb/xmb/glass.hpp"

#include "dialog_text_layout.hpp"
#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <cmath>

module openxmb.app;
import :message_overlay;

import auroreengine;
import :xmb_presentation;
import glm;
import spdlog;
import openxmb.config;

namespace app {
    namespace {
        float smooth01(float value) {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }
    }

    message_overlay::message_overlay(std::string title, std::string message, std::vector<std::string> choices,
        std::function<void(unsigned int)> confirm_callback, bool cancelable, std::function<void()> cancel_callback
    ) : title(std::move(title)), message(std::move(message)), choices(std::move(choices)),
        confirm_callback(std::move(confirm_callback)), cancelable(cancelable), cancel_callback(std::move(cancel_callback))
    {
        this->title=openxmb::localization::shell_locale::translate(this->title);
        this->message=openxmb::localization::shell_locale::translate(this->message);
        for(auto& label:this->choices) label=openxmb::localization::shell_locale::translate(label);
    }

    void message_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
        if(choices.empty()) {
            selected = 0;
        } else {
            selected = std::min<unsigned int>(selected, static_cast<unsigned int>(choices.size() - 1));
        }

        using clock = std::chrono::steady_clock;
        namespace glass = openxmb::xmb::glass;
        const auto now = clock::now();
        const float age_ms = std::chrono::duration<float, std::milli>(now - start_time).count();
        const float now_ms = std::chrono::duration<float, std::milli>(now.time_since_epoch()).count();
        const auto pose = glass::panel_in(age_ms);
        renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, pose.alpha));
        const glm::vec4 primary{1.0f, 1.0f, 1.0f, glass::text_primary};

        overlay_canvas c(renderer);
        const float vis_left = c.visible_left();
        const float vis_right = c.visible_right();
        const float vis_top = c.visible_top();
        const float vis_bottom = c.visible_bottom();
        const float span_w = vis_right - vis_left;
        const float span_h = vis_bottom - vis_top;
        const float vis_cx = (vis_left + vis_right) * 0.5f;
        const float vis_cy = (vis_top + vis_bottom) * 0.5f;

        c.rect(vis_left - 2.0f, vis_top - 2.0f, span_w + 4.0f, span_h + 4.0f, {0.0f, 0.0f, 0.0f, 0.30f});

        constexpr float margin = 40.0f;
        // Room for the hint bar below the card.
        constexpr float hint_reserve = 112.0f;
        const float max_card_w = std::max(280.0f, span_w - 2.0f * margin);
        const float max_card_h = std::max(240.0f, span_h - margin - hint_reserve);
        const float card_w = std::min(960.0f, max_card_w);
        const float content_width = std::max(160.0f, card_w - 120.0f);
        const float choice_row_width = std::max(160.0f, card_w - 80.0f);
        constexpr float choice_gap = 16.0f;
        const std::size_t choice_slots = std::max<std::size_t>(1, choices.size());
        const float choice_slot_w = std::min(360.0f,
            (choice_row_width - choice_gap * static_cast<float>(choice_slots - 1)) /
                static_cast<float>(choice_slots));

        const float ui=config::CONFIG.textScale;
        const float pixels=24*c.scale*ui;
        const auto font=config::CONFIG.fontPath.string();
        if(wrapped_pixels!=pixels||wrapped_font!=font) {
            wrapped_pixels=pixels;wrapped_font=font;
            title_lines=dialog_text::wrap(title,content_width*c.scale,[&](auto v){return renderer.text_width_pixels(v,28*c.scale*ui);});
            if(title_lines.size()>2) {title_lines.resize(2);title_lines.back()=dialog_text::ellipsize(title_lines.back()+"...",content_width*c.scale,[&](auto v){return renderer.text_width_pixels(v,28*c.scale*ui);});}
            body_lines=dialog_text::wrap(message,content_width*c.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
            choice_lines.clear();
            const float width=std::max(80.0f,choice_slot_w-36.0f)*c.scale;
            for(const auto& choice:choices) {
                auto lines=dialog_text::wrap(choice,width,[&](auto v){return renderer.text_width_pixels(v,26*c.scale*ui);});
                if(lines.size()>2){lines.resize(2);lines.back()=dialog_text::ellipsize(lines.back()+"...",width,[&](auto v){return renderer.text_width_pixels(v,26*c.scale*ui);});}
                choice_lines.push_back(std::move(lines));
            }
        }
        const auto title_metrics=renderer.text_line_metrics_pixels(28*c.scale*ui)/c.scale;
        const auto body_metrics=renderer.text_line_metrics_pixels(pixels)/c.scale;
        const auto choice_metrics=renderer.text_line_metrics_pixels(26*c.scale*ui)/c.scale;

        const float title_line_h = title_metrics.x + title_metrics.y;
        const float title_pitch = title_line_h + 8.0f * ui;
        const float title_block_h = title_lines.empty()
            ? 0.0f
            : title_line_h + static_cast<float>(title_lines.size() - 1) * title_pitch;

        const float body_line_h = body_metrics.x + body_metrics.y;
        const float body_pitch = body_line_h + 8.0f * ui;

        const float choice_line_h = choice_metrics.x + choice_metrics.y;
        const float choice_pitch = choice_line_h + 8.0f * ui;
        std::size_t max_choice_lines = 1;
        for(const auto& lines : choice_lines) {
            max_choice_lines = std::max(max_choice_lines, lines.size());
        }
        const float choice_text_h = choice_line_h + static_cast<float>(max_choice_lines - 1) * choice_pitch;
        const float choice_slot_h = choices.empty() ? 0.0f : std::max(64.0f, choice_text_h + 22.0f);

        constexpr float top_pad = glass::card_padding;
        const float title_body_gap = title_lines.empty() ? 0.0f : 16.0f;
        const float body_choice_gap = choices.empty() ? 0.0f : 24.0f;
        const float choices_footer_gap = 0.0f;
        constexpr float footer_h = glass::card_padding;

        const float fixed_h = top_pad + title_block_h + title_body_gap +
            body_choice_gap + choice_slot_h + choices_footer_gap + footer_h;
        const float max_body_h = std::max(body_line_h, max_card_h - fixed_h);
        const std::size_t capacity = dialog_text::line_capacity(max_body_h, body_pitch, body_line_h);

        line_count = body_lines.size();
        visible_lines = std::min(std::max<std::size_t>(1, line_count), capacity);
        scroll_line = std::min(scroll_line, line_count > visible_lines ? line_count - visible_lines : 0);

        const float body_h = body_line_h + static_cast<float>(visible_lines - 1) * body_pitch;
        const float card_h = std::min(max_card_h, fixed_h + body_h);
        const float card_x = vis_cx - card_w * 0.5f;
        const float card_y = std::clamp(vis_cy - card_h * 0.5f - hint_reserve * 0.5f, vis_top + margin,
            std::max(vis_top + margin, vis_bottom - hint_reserve - card_h));

        c.glass(vis_cx - card_w * pose.scale * 0.5f, card_y + card_h * (1.0f - pose.scale) * 0.5f,
            card_w * pose.scale, card_h * pose.scale);

        const float title_top = card_y + top_pad;
        for(std::size_t i = 0; i < title_lines.size(); ++i) {
            c.text(title_lines[i], vis_cx, title_top + title_metrics.x + static_cast<float>(i) * title_pitch,
                28, primary, 0.0f, true);
        }

        const float body_top = title_top + title_block_h + title_body_gap;
        const float body_bottom = body_top + body_h;
        for(std::size_t i = 0; i < visible_lines && i + scroll_line < line_count; ++i) {
            c.text(body_lines[i + scroll_line], vis_cx, body_top + body_metrics.x + static_cast<float>(i) * body_pitch,
                24, primary, 0.0f, true);
        }

        if(line_count > visible_lines) {
            const float arrow_x = vis_cx + content_width * 0.5f + 24.0f;
            if(scroll_line > 0) {
                c.arrow(arrow_x, body_top + 10.0f, 14.0f, xmb_canvas::arrow_direction::up, {1.0f, 1.0f, 1.0f, 0.85f});
            }
            if(scroll_line + visible_lines < line_count) {
                c.arrow(arrow_x, body_bottom - 10.0f, 14.0f, xmb_canvas::arrow_direction::down, {1.0f, 1.0f, 1.0f, 0.85f});
            }
        }

        if(!choices.empty()) {
            const float total_row_w = static_cast<float>(choices.size()) * choice_slot_w +
                static_cast<float>(choices.size() - 1) * choice_gap;
            const float row_x = vis_cx - total_row_w * 0.5f;
            const float slot_y = body_bottom + body_choice_gap;
            // The focus capsule slides between choices.
            focus.move_to({row_x + static_cast<float>(selected) * (choice_slot_w + choice_gap), slot_y, choice_slot_w, choice_slot_h}, now_ms);
            const auto lit = focus.at(now_ms);
            c.glass(lit.x, lit.y, lit.w, lit.h, glass::variant::focus, lit.h * 0.5f);
            for(std::size_t i = 0; i < choices.size(); ++i) {
                const float sx = row_x + static_cast<float>(i) * (choice_slot_w + choice_gap);
                const glm::vec4 col = primary;
                const auto& lines = choice_lines[i];
                const float ch_h = choice_line_h + static_cast<float>(std::max<std::size_t>(1, lines.size()) - 1) * choice_pitch;
                const float first_base = slot_y + (choice_slot_h - ch_h) * 0.5f + choice_metrics.x;
                for(std::size_t line = 0; line < lines.size(); ++line) {
                    c.text(lines[line], sx + choice_slot_w * 0.5f, first_base + static_cast<float>(line) * choice_pitch,
                        26, col, 0.0f, true);
                }
            }
        }

        const auto previous_scale = renderer.text_scale;
        renderer.text_scale = 1.0f;
        xmb->render_hint_bar(renderer, std::array{
            std::pair{action::ok, choices.empty() ? "OK" : "Select"},
            std::pair{cancelable ? action::cancel : action::none, "Back"}
        });
        renderer.text_scale = previous_scale;
        renderer.pop_color();
    }

    result message_overlay::on_action(action action) {
        switch(action) {
            case action::up:
                if(scroll_line) --scroll_line;
                return result::success;
            case action::down:
                if(scroll_line+visible_lines<line_count) ++scroll_line;
                return result::success;
            case action::cancel:
                if(cancelable) {
                    if(cancel_callback) {
                        cancel_callback();
                    }
                    return result::success | result::close | result::cancel_sound;
                }
                return result::success;
            case action::ok:
                if(confirm_callback) {
                    confirm_callback(selected);
                }
                return result::success | result::close | result::confirm_sound;
            case action::left:
                if(choices.empty()) {
                    return result::success | result::error_rumble;
                }
                if(selected > 0) {
                    selected--;
                    return result::success | result::ok_sound;
                }
                return result::success | result::error_rumble;
            case action::right:
                if(choices.empty()) {
                    return result::success | result::error_rumble;
                }
                if(selected + 1 < choices.size()) {
                    selected++;
                    return result::success | result::ok_sound;
                }
                return result::success | result::error_rumble;
            default:
                return result::success;
        }
    }
}
