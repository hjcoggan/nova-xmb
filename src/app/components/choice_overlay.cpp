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
#include "openxmb/xmb/scrolling_text.hpp"

#include "dialog_text_layout.hpp"
#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module openxmb.app;
import :choice_overlay;
import :xmb_presentation;
import openxmb.config;

import auroreengine;
import glm;
import openxmb.utils;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback,
    std::function<void()> cancel_callback,
    std::function<void(unsigned int)> preview_callback)
    : choices{std::move(choices)}, confirm_callback{std::move(confirm_callback)},
      cancel_callback{std::move(cancel_callback)},
      preview_callback{std::move(preview_callback)}
{
    if(this->choices.empty()) {
        this->selection_index = 0;
        this->last_selection_index = 0;
    } else {
        this->selection_index = std::min<unsigned int>(selection_index, static_cast<unsigned int>(this->choices.size() - 1));
        this->last_selection_index = this->selection_index;
    }
}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close | result::back_sound;
        case action::ok:
            if(choices.empty()) {
                return result::success | result::error_rumble;
            }
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close | result::confirm_sound;
        case action::left:
            text_scroll_line=text_scroll_line>selected_page_lines?text_scroll_line-selected_page_lines:0;
            return result::success;
        case action::right:
            if(selected_total_lines>selected_page_lines)text_scroll_line=std::min(text_scroll_line+selected_page_lines,selected_total_lines-selected_page_lines);
            return result::success;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::success | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::success | result::error_rumble;
        default:
            return result::success | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
    if(choices.empty()) {
        return false;
    }

    if(dir == action::up) {
        if(selection_index <= 0) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + choices.size() - 1) % choices.size();
    } else if(dir == action::down) {
        if(selection_index >= choices.size() - 1) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + 1) % choices.size();
    } else {
        return false;
    }
    text_scroll_line=0;
    if(preview_callback) {
        preview_callback(selection_index);
    }
    return true;
}

void choice_overlay::render(auroreengine::gui_renderer& renderer, class shell* xmb) {
    namespace glass = openxmb::xmb::glass;

    // A glass sheet on the right, in place of the firmware's lavender wash:
    // the menu stays visible beside it, slightly dimmed, and the list sits
    // on smoked glass so it reads over any wallpaper.
    const auto steady_now = std::chrono::steady_clock::now();
    const float now_ms = std::chrono::duration<float, std::milli>(steady_now.time_since_epoch()).count();
    const auto pose = glass::panel_in(std::chrono::duration<float, std::milli>(steady_now - opened_time).count());
    const float open_alpha = pose.alpha;
    overlay_canvas canvas(renderer);
    // Room for the hint bar below the sheet.
    constexpr float hint_reserve = 112.0F;
    constexpr float panel_right = 1860.0F / 1920.0F;
    const float panel_edge_authored = (panel_right * static_cast<float>(renderer.frame_size.width) - canvas.ox) / canvas.scale;
    const float x = panel_edge_authored - 357.0F;
    canvas.rect(canvas.visible_left() - 2, canvas.visible_top() - 2,
        canvas.visible_right() - canvas.visible_left() + 4, canvas.visible_bottom() - canvas.visible_top() + 4,
        {0.0F, 0.01F, 0.04F, 0.30F * open_alpha});

    // x already computed above with overlay_canvas so the panel and text share bounds.
    const float ui=config::CONFIG.textScale;
    const float pixels=28*canvas.scale*ui;
    const auto font=config::CONFIG.fontPath.string();
    const auto metrics=renderer.text_line_metrics_pixels(pixels)/canvas.scale;
    const float line_height=metrics.x+metrics.y,pitch=line_height+8*ui;
    constexpr float available_width=337;
    if(wrapped_pixels!=pixels||wrapped_font!=font) {
        wrapped_pixels=pixels;wrapped_font=font;
        wrapped_choices.clear();row_heights.clear();
        for(const auto& entry:choices) {
            // One row is one line. Long names (SSIDs, Bluetooth devices)
            // used to wrap into several lines and scroll vertically; they
            // now use the one horizontal scrolling-text slot instead.
            wrapped_choices.push_back({entry});
            row_heights.push_back(line_height+12*ui);
        }
    }
    std::vector<std::string> title_labels;
    float top = 518.0F - metrics.x;
    float sheet_top = top - 34.0F;
    if(!top_action.empty()) {
        // A sheet title is one line. Wrapping it made "Select Wi-Fi Network"
        // two ragged lines and pushed the first row down unpredictably.
        const float title_pixels = 24.0F * canvas.scale * ui;
        const auto title_metrics = renderer.text_line_metrics_pixels(title_pixels) / canvas.scale;
        title_labels = dialog_text::wrap(top_action, available_width * canvas.scale,
            [&](auto v){ return renderer.text_width_pixels(v, title_pixels); });
        if(title_labels.size() > 1) {
            title_labels.resize(1);
            title_labels.front() = dialog_text::ellipsize(title_labels.front() + "...", available_width * canvas.scale,
                [&](auto v){ return renderer.text_width_pixels(v, title_pixels); });
        }
        const float baseline = top_action_y * 1080.0F;
        sheet_top = (baseline - title_metrics.x) - 34.0F;
        top = baseline + title_metrics.y + 24.0F * ui;
    }
    {
        // Size the sheet to its list: a title if there is one, then the rows,
        // never taller than the screen.
        float rows_height = 0.0F;
        for(const auto row : row_heights) rows_height += row;
        const float sheet_bottom = std::min(canvas.visible_bottom() - hint_reserve, top + rows_height + glass::card_padding);
        const float sheet_w = 357.0F + 58.0F, sheet_h = sheet_bottom - sheet_top;
        canvas.glass(x - 36.0F + sheet_w * (1.0F - pose.scale) * 0.5F, sheet_top + sheet_h * (1.0F - pose.scale) * 0.5F,
            sheet_w * pose.scale, sheet_h * pose.scale, glass::variant::regular, glass::card_radius, open_alpha);
    }
    if(!title_labels.empty()) {
        const float baseline = top_action_y * 1080.0F;
        for(std::size_t i = 0; i < title_labels.size(); ++i) {
            canvas.text(title_labels[i], x, baseline + static_cast<float>(i) * pitch, 24, {1, 1, 1, glass::text_secondary * open_alpha});
        }
    }
    const float bottom=std::min(1000.0F,canvas.visible_bottom()-hint_reserve-glass::card_padding),viewport=std::max(line_height,bottom-top);
    const auto window=dialog_text::choice_window(row_heights,selection_index,viewport,first_visible);
    first_visible=window.first;
    selected_page_lines=dialog_text::line_capacity(viewport,pitch,line_height);
    selected_total_lines=wrapped_choices.empty()?0:wrapped_choices[selection_index].size();
    text_scroll_line=std::min(text_scroll_line,selected_total_lines>selected_page_lines?selected_total_lines-selected_page_lines:0);
    renderer.set_clip((canvas.ox+(x-24.0F)*canvas.scale)/renderer.frame_size.width,
        (canvas.oy+(top-12.0F)*canvas.scale)/renderer.frame_size.height,
        (357.0F+34.0F)*canvas.scale/renderer.frame_size.width,(viewport+24.0F)*canvas.scale/renderer.frame_size.height);
    float y=top+metrics.x;
    const float swatch_inset=swatches.empty()?0.0F:44.0F;
    {
        // One lit capsule, sliding to the selected row.
        float row_y=top+metrics.x;
        for(std::size_t i=window.first;i<window.last&&i<selection_index;++i) row_y+=row_heights[i];
        if(selection_index>=window.first&&selection_index<window.last) {
            const float row_height=row_heights[selection_index]-12*ui;
            focus.move_to({x-18.0F,row_y-metrics.x-9.0F,available_width+30.0F,row_height+18.0F},now_ms);
            const auto lit=focus.at(now_ms);
            canvas.glass(lit.x,lit.y,lit.w,lit.h,glass::variant::focus,std::min(lit.h*0.5F,glass::concentric_radius(glass::card_radius,18.0F)),open_alpha);
        }
    }
    for(std::size_t i=window.first;i<window.last;++i) {
        const bool focused=i==selection_index;
        if(i<swatches.size()) {
            canvas.rect(x-2.0F,y-metrics.x+(line_height-28.0F)*0.5F-1.0F,30,30,{1,1,1,0.55F*open_alpha},8);
            canvas.rect(x,y-metrics.x+(line_height-28.0F)*0.5F+1.0F,26,26,{swatches[i],open_alpha},6);
        }
        const auto& lines=wrapped_choices[i];
        const std::string_view line=lines.empty()?std::string_view{}:std::string_view(lines.front());
        const auto measure=[&](std::string_view text){
            return renderer.text_width_pixels(text,pixels)/canvas.scale;
        };
        const auto fitting=openxmb::xmb::ui::fitting_count(
            line,static_cast<float>(available_width)-swatch_inset,measure);
        if(focused) {
            if(focus_scroll_text!=line||!focus_scroll.focused()) {
                focus_scroll_text.assign(line);
                focus_scroll.set_text(focus_scroll_text,fitting,true,now_ms);
            }
            const auto glyph=fitting?measure(line.substr(0,1)):1.0F;
            const auto run=focus_scroll.sample(now_ms,glyph);
            canvas.text(line.substr(run.first,run.count),
                x+swatch_inset-run.offset_px,y,26,{1,1,1,glass::text_primary*open_alpha});
        } else {
            auto shown=std::string(line);
            if(fitting>0&&shown.size()>fitting) {
                shown.resize(fitting);
                while(!shown.empty()&&(static_cast<unsigned char>(shown.back())&0xC0)==0x80) shown.pop_back();
                shown=shown.empty()?"...":shown+"...";
            }
            canvas.text(shown,x+swatch_inset,y,26,{1,1,1,glass::text_primary*open_alpha});
        }
        y+=row_heights[i];
    }
    renderer.reset_clip();
    // The font has no arrow glyphs; the markers are drawn.
    using direction=xmb_canvas::arrow_direction;
    const float centre=x+available_width*0.5F-12.0F;
    if(window.first)canvas.arrow(centre,top-20.0F,16.0F,direction::up,{1,1,1,0.85F*open_alpha});
    if(window.last<choices.size())canvas.arrow(centre,top+viewport+20.0F,16.0F,direction::down,{1,1,1,0.85F*open_alpha});
    if(selected_total_lines>selected_page_lines) {
        const float y_more=bottom+20.0F;
        canvas.arrow(x+6.0F,y_more-6.0F,12.0F,direction::left,{1,1,1,open_alpha});
        canvas.text("More text",x+20.0F,y_more,16,{1,1,1,open_alpha});
        canvas.arrow(x+112.0F,y_more-6.0F,12.0F,direction::right,{1,1,1,open_alpha});
    }
    renderer.push_color({1,1,1,open_alpha});
    xmb->render_hint_bar(renderer,std::array{std::pair{action::ok,"Select"},std::pair{action::cancel,"Back"}});
    renderer.pop_color();
}

}
